module;

import lx.foundation;

export module lx.gfx:ui_cpu;

import std;

import :primitives;
import :atlas;
import :damage;
import :renderer;
import :cpu_renderer;

// Bit-identical output across hosts is the point of the reference renderer: no fused
// multiply-add (a host with FMA would round differently), and no libm transcendental — the
// few used are written out below (`ui_math`). `std::sqrt` is IEEE-exact and stays.
#pragma clang fp contract(off)

export namespace lx::gfx {

// ── The UI primitive renderer, CPU reference (ADR-010 §4.4, Phase 1) ────────────────
//
// Rasterises one view's sorted primitive stream (`scene::draw_list::batch_for_render`)
// into a CPU-addressable target, inside the damage rects only. It is the reference the GPU
// backends (`:ui_gl`, `:ui_vk`) are compared against (G10: within 3/255 per channel), and
// the CPU backend where there is no GPU. Every rule below is what those backends copy.
//
// Target. XRGB8888 or ARGB8888 (memory B, G, R, X/A), premultiplied, sRGB-encoded. Colours
// blend in that encoded space (ADR-010 §4.5, GPUI's look): premultiplied source-over,
// out = src + dst · (1 − src.a), per channel in float from the byte (dst / 255), rounded
// to the nearest byte, once per primitive per pixel — the arithmetic an 8-bit UNORM GPU
// target does. XRGB's fourth byte is treated as 255 and written 255.
//
// Coverage domain. Coordinates are output pixels; a pixel's sample point is its centre
// (x + ½, y + ½). A primitive writes a pixel only when that centre lies inside its raster
// bounds and inside its content mask (`clip`, x ≤ c < x + w — the top-left rule a GPU
// rasteriser uses; a clip of zero width or height is no mask), and inside the damage rect
// being drawn. Raster bounds are `bounds` for quads, sprites, underlines and inset
// shadows, and `bounds` grown by 3σ + spread on every side for outer shadows.
//
// quad. GPUI's rounded-box SDF. For centre c, half-size h, and the corner radius r picked by
// quadrant (p.x < c.x: left; p.y < c.y: top; ties go right / bottom), clamped to
// [0, min(h.x, h.y)]: q = |p − c| − h + r; d = r == 0 ? max(q.x, q.y)
// : length(max(q, 0)) + min(max(q.x, q.y), 0) − r. Coverage a = saturate(½ − d).
//   Border: the inner box is the outer box inset by the widths (top, right, bottom, left);
// inner radii are max(0, r − max(the two adjacent widths)), clamped to the inner box. With
// a_o the outer and a_i the inner coverage (0 when the inner box is empty),
// w_i = min(a_i, a_o) and w_b = a_o − w_i; the colour is
// background · w_i + (border over background) · w_b, where border over background =
// border + background · (1 − border.a): the background fills the inner box, the ring shows
// the border composited over the background (a translucent border shows the background
// through it, as GPUI and CSS `background-clip: border-box` do). A quad with every width 0
// is background · a_o.
//   Dashed borders (border_style 1): corners are solid — the corner zone of the top-left
// corner is x < x0 + max(r_tl, w_left) ∧ y < y0 + max(r_tl, w_top), likewise for the
// others. Elsewhere the pixel belongs to the edge whose band it is least outside of (the
// edge minimising distance-from-the-edge − width, over edges of non-zero width). Along that
// edge, t is measured clockwise from the end of the corner zone it starts at (top: from
// the top-left zone rightwards; right: from the top-right zone down; bottom: from the
// bottom-right zone leftwards; left: from the bottom-left zone up). With w that edge's
// width the dash period is 4w — a dash 2w long, then a gap 2w long, starting with a dash —
// and m = t mod 4w; the dash coverage is saturate(½ + (m < 2w ? min(m, 2w − m)
// : −min(m − 2w, 4w − m))), multiplying the border colour (the gap shows the background).
//   Gradient (background_end differs from background, bit for bit): linear from background to
// background_end. The angle is in degrees, 0 = left → right, 90 = top → bottom (clockwise,
// y down): direction v = (cos θ, sin θ); t = saturate(½ + ((p − c) · v) / L) with
// L = |w cos θ| + |h sin θ| (the box's extent along v: the corners reach 0 and 1). Space 0
// interpolates the premultiplied encoded colours linearly; space 1 interpolates in Oklab
// (each stop un-premultiplied, decoded with the sRGB EOTF, to Oklab; L, a, b and alpha
// interpolated; back to linear, clamped to [0, 1], encoded, premultiplied).
//
// shadow. Evan Wallace's closed-form rounded-box shadow, as GPUI's shadow shader: the box is
// `bounds` grown by `spread` (radii grown by it too, then clamped); for the pixel centre
// p relative to the box centre, the corner radius r by quadrant as for quads,
// low = p.y − h.y, high = p.y + h.y, start = clamp(−3σ, low, high),
// end = clamp(3σ, low, high), step = (end − start) / 4, and four samples at
// y_i = start + (i + ½) · step: mask = Σ blur_x(p.x, p.y − y_i) · g(y_i) · step, where
// g(y) = exp(−y² / 2σ²) / (√(2π) σ), blur_x(x, y) = ½ (erf((x + k) / (√2 σ)) −
// erf((x − k) / (√2 σ))), k = h.x − r + √max(0, r² − δ²), δ = min(h.y − r − |y|, 0), and
// erf(x) ≈ s − s / (1 + (0.278393 + (0.230389 + 0.078108 a²) a) a)⁴, s = sign(x), a = |x|.
// σ below 1/10000 px is a hard shadow: mask = saturate(½ − d) of the box. Colour =
// colour · mask. Inset (flags bit 0): the element box is `bounds` (its radii), the hole is
// `bounds` shrunk by `spread` (radii − spread, at least 0); mask = coverage(element box) ·
// (1 − shadow mask of the hole); an empty hole shadows the whole element.
//
// mono_sprite / path. An R8 atlas page × colour: colour · coverage, coverage = texel / 255.
// The texel is the pixel's centre mapped linearly from `bounds` onto the tile (`uv`, atlas
// texels). Placement on whole pixels at 1:1 (integer bounds origin, bounds size = tile
// size) samples the nearest texel exactly; anything else is bilinear at (u − ½, v − ½)
// with the sample point clamped to the tile's texel centres, so nothing outside the tile
// is ever read. `params` (the rotation) is not applied in Phase 1. path has no `params`.
// Flags bits 1–3: an output transform (`lx::buffer_transform`, 0 normal) the view turned the
// sprite by (`draw_list::transform_primitives`): `bounds` is then the turned box and the
// pixel's centre is mapped back into the unturned one through `source_basis_for` — the map
// a blit's buffer transform samples through — before the rules above (whole pixels at 1:1
// compare the unturned size with the tile).
//
// poly_sprite. An RGBA8 page (premultiplied, R, G, B, A bytes), sampled as above, × the
// instance colour component-wise (a premultiplied multiplier: white at alpha o fades to
// o). Flags bit 0: grayscale — r = g = b = 0.2126 r + 0.7152 g + 0.0722 b of the sample
// (premultiplied, encoded), before the multiply. `params` are corner radii: the result is
// multiplied by the quad coverage of `bounds` with those radii.
//
// A sprite whose page is not created, or whose page format does not match its kind (mono
// and path need R8, poly needs RGBA8), is skipped and counted.
//
// underline. A horizontal line centred on the bounds' vertical middle y_c, thickness t.
// Straight: coverage = saturate(t/2 − |p.y − y_c| + ½). Wavy (wavy ≠ 0): the curve is
// f(x) = y_c + t · sin(2π (p.x − x0) / 4t) — amplitude t, period 4t, starting at the
// bounds' left edge — the distance is |p.y − f| / √(1 + f′²) and coverage = saturate(t/2 −
// distance + ½). Colour · coverage. The bounds clip the curve.
//
// surface. Drawn by `cpu_compositor::blend` exactly as the compositor draws a
// `blit_command` (linear-light blending, its filters and transforms), when a compositor
// with the textures registered was set; otherwise skipped and counted.
//
// Order and damage. For each damage rect (a full region is one rect, the whole target),
// clipped to the target: clear it to the clear colour (when given), then every batch whose
// bounds meet the rect, in order, each batch's refs in order. Overlapping rects repaint
// their overlap; the result is the same. Pixels outside every rect are never read or
// written. (Drawing starts at the topmost ref that repaints the band opaquely — an opaque
// square quad on whole pixels, or a surface `cpu_compositor::covers_opaquely` — with no
// clear: the same pixels either way.)

/// One view's sorted stream as the scene publishes it (`scene::draw_list`): the refs and
/// batches of `batch_for_render`, the typed instance arrays the refs index, and the surface
/// draws `surface` refs index (the scene's draw commands converted, as the compositor does
/// for its backends). Borrowed for the draw.
struct ui_stream {
    std::span<const primitive_ref> refs{};
    std::span<const batch> batches{};
    std::span<const quad_instance> quads{};
    std::span<const shadow_instance> shadows{};
    std::span<const sprite_instance> sprites{};
    std::span<const underline_instance> underlines{};
    std::span<const blit_command> surfaces{};
};

struct ui_draw_stats {
    unsigned rects = 0;             ///< damage rects drawn
    unsigned batches_drawn = 0;     ///< (batch, band) pairs that met (a band: rows of a rect)
    unsigned batches_skipped = 0;   ///< (batch, band) pairs whose bounds missed
    unsigned primitives_drawn = 0;  ///< (primitive, band) pairs rasterised
    unsigned primitives_skipped = 0; ///< bad index, missing or mismatched atlas page, no compositor
    /// What `cpu_compositor::blend` reported for the surface runs, summed over (run, band)
    /// pairs: draws of an unregistered texture, of a format with no conversion to the
    /// target's, and draws culled (occluded or invisible).
    unsigned surface_draws_skipped = 0;
    unsigned surface_draws_unsupported = 0;
    unsigned surface_draws_culled = 0;
    unsigned long long pixels_shaded = 0;
};

/// Refs per stream the band loop indexes (the scene's caps: primitives plus surfaces); a
/// longer stream is drawn rect by rect without bands — the same pixels, more cache misses.
inline constexpr unsigned k_ui_cpu_max_refs = k_max_primitives + 4096;
/// A damage rect is drawn in bands of about this many pixels (128 KiB of target, which
/// stays in L2 while every primitive crossing the band is drawn into it). Bands are rows
/// of the rect, disjoint: the pixels are those of drawing the rect at once.
inline constexpr unsigned k_ui_cpu_band_pixels = 32768;

class ui_cpu_renderer {
public:
    ui_cpu_renderer();

    /// Where `surface` refs' textures are registered. Borrowed; null skips surfaces.
    void set_surface_compositor(cpu_compositor* compositor) { compositor_ = compositor; }
    /// The quad interior fast path (a span of fully covered, border-free pixels written
    /// without the SDF). On by default; the output is bit-identical either way — off is for
    /// the test that proves it.
    void set_fast_paths(bool on) { fast_paths_ = on; }

    /// Rasterises `stream` into `target` (XRGB8888 or ARGB8888) inside `damage`, clearing
    /// each damage rect to `clear` (premultiplied, encoded) first when one is given. An
    /// empty region draws nothing. `atlas` may be null when the stream has no sprites.
    [[nodiscard]] lx::result<ui_draw_stats> draw(const pixel_surface& target,
                                                 const ui_stream& stream, const atlas* atlas,
                                                 const damage_region& damage,
                                                 std::optional<lx::color> clear = std::nullopt);

private:
    [[nodiscard]] lx::result<void> draw_band(const void* frame, const ui_stream& stream,
                                             const atlas* atlas, lx::rect2i band, bool banded,
                                             std::optional<std::uint32_t> clear_word);

    cpu_compositor* compositor_ = nullptr;
    bool fast_paths_ = true;
    /// Per sorted ref: the first and one-past-last row it can write (2 per ref).
    std::unique_ptr<std::int32_t[]> rows_;
};

/// What `lower_to_blits` did with a stream.
struct ui_lower_stats {
    unsigned surfaces = 0; ///< `surface` refs copied
    unsigned lowered = 0;  ///< quads turned into solid blits
    unsigned dropped = 0;  ///< primitives with no blit equivalent: counted, not drawn
    unsigned overflow = 0; ///< refs past `capacity`
};

/// True when `q` is a solid rectangle on whole pixels — every radius 0, no visible border
/// (every width 0, or a fully transparent border colour), `background_end` equal to
/// `background` bit for bit, `bounds` and `clip` integral — which `:ui_cpu` fills with one
/// colour at full coverage: a `blit_command::solid` draws the same pixels.
[[nodiscard]] bool quad_lowers_to_blit(const quad_instance& q);

/// The stream as `blit_command`s in its sorted order, for a path with no primitive renderer
/// (the compositor's GLES 2 and Vulkan composites, screen capture's re-composites, the
/// headless dump): a `surface` ref is copied from `stream.surfaces`; a quad that
/// `quad_lowers_to_blit` becomes a solid blit of its premultiplied encoded colour
/// (`blend_mode::opaque` at alpha 1, else premultiplied source-over in the backend's own
/// blend space); every other primitive is dropped and counted in `stats.dropped`. Writes at
/// most `capacity` commands (the rest counted in `stats.overflow`) and returns how many. No
/// allocation; any affinity (it reads the stream only).
[[nodiscard]] unsigned lower_to_blits(const ui_stream& stream, blit_command* out,
                                      unsigned capacity, ui_lower_stats& stats);

} // namespace lx::gfx

// ── Deterministic elementary functions ──────────────────────────────────────────
//
// Plain float arithmetic only, so the same binary gives the same bits on every x86-64 (and
// the same source gives the same bits on any IEEE host without FMA contraction). Accuracy
// is ~1e-7 relative over the ranges used, well inside what an 8-bit target can show.

namespace lx::gfx::ui_math {

inline constexpr float k_pi = 3.14159265358979323846f;

/// 2^x for x in [−126, 127].
[[nodiscard]] inline float exp2(float x) {
    if (x < -126.f)
        return 0.f;
    if (x > 127.f)
        x = 127.f;
    const float n = std::floor(x);
    const float f = x - n; // [0, 1)
    // e^(f ln 2) by Taylor to the 9th term: relative error < 3e-8 on [0, ln 2).
    const float u = f * 0.69314718056f;
    float term = 1.f;
    float sum = 1.f;
    for (int k = 1; k <= 9; ++k) {
        term = term * u / static_cast<float>(k);
        sum = sum + term;
    }
    const auto e = static_cast<std::int32_t>(n) + 127;
    return sum * std::bit_cast<float>(static_cast<std::uint32_t>(e) << 23u);
}

[[nodiscard]] inline float exp(float x) { return exp2(x * 1.44269504089f); }

/// log2(x) for x > 0 (finite, normal).
[[nodiscard]] inline float log2(float x) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(x);
    int e = static_cast<int>((bits >> 23u) & 0xFFu) - 127;
    float m = std::bit_cast<float>((bits & 0x7FFFFFu) | 0x3F800000u); // [1, 2)
    if (m > 1.41421356f) {
        m = m * 0.5f;
        ++e;
    }
    // ln m = 2 atanh(s), s = (m − 1) / (m + 1), |s| ≤ 0.172.
    const float s = (m - 1.f) / (m + 1.f);
    const float s2 = s * s;
    float term = s;
    float sum = s;
    for (int k = 3; k <= 13; k += 2) {
        term = term * s2;
        sum = sum + term / static_cast<float>(k);
    }
    return static_cast<float>(e) + 2.f * sum * 1.44269504089f;
}

[[nodiscard]] inline float pow(float x, float y) {
    if (x <= 0.f)
        return 0.f;
    return exp2(y * log2(x));
}

[[nodiscard]] inline float cbrt(float x) {
    if (x == 0.f)
        return 0.f;
    return x < 0.f ? -pow(-x, 1.f / 3.f) : pow(x, 1.f / 3.f);
}

/// sin(x) for any finite x.
[[nodiscard]] inline float sin(float x) {
    // Reduce to [−π, π], then to [−π/2, π/2] by sin(π − x) = sin(x).
    const float turns = std::floor(x / (2.f * k_pi) + 0.5f);
    x = x - turns * (2.f * k_pi);
    if (x > k_pi * 0.5f)
        x = k_pi - x;
    else if (x < -k_pi * 0.5f)
        x = -k_pi - x;
    const float x2 = x * x;
    float term = x;
    float sum = x;
    for (int k = 2; k <= 12; k += 2) {
        term = -term * x2 / static_cast<float>(k * (k + 1));
        sum = sum + term;
    }
    return sum;
}

[[nodiscard]] inline float cos(float x) { return sin(x + k_pi * 0.5f); }

[[nodiscard]] inline float srgb_decode(float v) {
    if (v <= 0.04045f)
        return v / 12.92f;
    return pow((v + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] inline float srgb_encode(float v) {
    if (v <= 0.0031308f)
        return v * 12.92f;
    return 1.055f * pow(v, 1.f / 2.4f) - 0.055f;
}

/// The erf approximation of Evan Wallace's shadow (Abramowitz–Stegun 7.1.27).
[[nodiscard]] inline float erf(float x) {
    const float s = x < 0.f ? -1.f : (x > 0.f ? 1.f : 0.f);
    const float a = std::fabs(x);
    float t = 1.f + (0.278393f + (0.230389f + 0.078108f * (a * a)) * a) * a;
    t = t * t;
    return s - s / (t * t);
}

} // namespace lx::gfx::ui_math

// ── Rasterisation ───────────────────────────────────────────────────────────────

namespace lx::gfx::ui_raster {

struct rgba {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 0.f;
};

[[nodiscard]] inline rgba scale(rgba c, float k) { return {c.r * k, c.g * k, c.b * k, c.a * k}; }
[[nodiscard]] inline rgba add(rgba x, rgba y) { return {x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a}; }
[[nodiscard]] inline rgba over(rgba top, rgba under) { return add(top, scale(under, 1.f - top.a)); }

[[nodiscard]] inline float saturate(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
[[nodiscard]] inline float fmin(float a, float b) { return a < b ? a : b; }
[[nodiscard]] inline float fmax(float a, float b) { return a > b ? a : b; }

[[nodiscard]] inline rgba half4(const std::uint16_t (&h)[4]) {
    return {from_half(h[0]), from_half(h[1]), from_half(h[2]), from_half(h[3])};
}

/// d / 255 for every byte: the float a GPU reads from a UNORM target.
struct unorm_table {
    float v[256]{};
    unorm_table() {
        for (unsigned i = 0; i < 256; ++i)
            v[i] = static_cast<float>(i) / 255.f;
    }
};

[[nodiscard]] inline const unorm_table& unorm() {
    static const unorm_table t{};
    return t;
}

/// Nearest byte of a [0, 1] value; below 0 (and NaN) is 0, above 1 is 255. Branch-free.
[[nodiscard]] inline std::uint32_t to_byte(float v) {
    v = v > 0.f ? v : 0.f;
    v = v < 1.f ? v : 1.f;
    return static_cast<std::uint32_t>(v * 255.f + 0.5f);
}

/// The target word of a premultiplied colour (B, G, R, A in memory).
[[nodiscard]] inline std::uint32_t pack(rgba c, bool has_alpha) {
    return ((has_alpha ? to_byte(c.a) : 255u) << 24u) | (to_byte(c.r) << 16u) |
           (to_byte(c.g) << 8u) | to_byte(c.b);
}

/// Premultiplied source-over in the target's encoded space, one byte rounding.
inline void blend(std::uint32_t& px, rgba s, bool has_alpha) {
    if (!(s.a > 0.f) && !(s.r > 0.f) && !(s.g > 0.f) && !(s.b > 0.f))
        return; // adds nothing: d · 1 rounds back to d
    if (s.a == 1.f) {
        // Opaque: d · 0 adds nothing, so the result is the source's bytes — written without
        // reading the target (a scanout dumb buffer is write-combining: a read is uncached).
        px = pack(s, has_alpha);
        return;
    }
    const float* u = unorm().v;
    const float k = 1.f - s.a;
    const std::uint32_t r = to_byte(s.r + u[(px >> 16u) & 0xFFu] * k);
    const std::uint32_t g = to_byte(s.g + u[(px >> 8u) & 0xFFu] * k);
    const std::uint32_t b = to_byte(s.b + u[px & 0xFFu] * k);
    const std::uint32_t a = has_alpha ? to_byte(s.a + u[px >> 24u] * k) : 255u;
    px = (a << 24u) | (r << 16u) | (g << 8u) | b;
}

/// Pixels [x0, x1) × [y0, y1).
struct window {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
};

/// The pixels whose centre lies in [lo, hi): [⌈lo − ½⌉, ⌈hi − ½⌉).
/// A float pixel coordinate as an int, clamped to ±10⁶ (NaN to −10⁶): an instance may
/// carry anything, and an out-of-range conversion is undefined.
[[nodiscard]] inline int clamp_px(float v) {
    if (!(v >= -1.e6f))
        return -1000000;
    if (!(v <= 1.e6f))
        return 1000000;
    return static_cast<int>(v);
}

inline void centres(float lo, float hi, int& a, int& b) {
    a = clamp_px(std::ceil(lo - 0.5f));
    b = clamp_px(std::ceil(hi - 0.5f));
}

/// Pixels of `rect` (the damage rect, already inside the target) whose centre is in the
/// raster bounds (x, y, w, h) and in `clip` (no mask when empty).
[[nodiscard]] inline window raster_window(float x, float y, float w, float h, const float (&clip)[4],
                                          lx::rect2i rect) {
    window out{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
    int a = 0;
    int b = 0;
    centres(x, x + w, a, b);
    out.x0 = a > out.x0 ? a : out.x0;
    out.x1 = b < out.x1 ? b : out.x1;
    centres(y, y + h, a, b);
    out.y0 = a > out.y0 ? a : out.y0;
    out.y1 = b < out.y1 ? b : out.y1;
    if (clip[2] > 0.f && clip[3] > 0.f) {
        centres(clip[0], clip[0] + clip[2], a, b);
        out.x0 = a > out.x0 ? a : out.x0;
        out.x1 = b < out.x1 ? b : out.x1;
        centres(clip[1], clip[1] + clip[3], a, b);
        out.y0 = a > out.y0 ? a : out.y0;
        out.y1 = b < out.y1 ? b : out.y1;
    }
    return out;
}

/// A rounded box: centre, half size, radii (top-left, top-right, bottom-right,
/// bottom-left) clamped to [0, min(half size)].
struct rbox {
    float cx = 0.f;
    float cy = 0.f;
    float hw = 0.f;
    float hh = 0.f;
    float r[4]{};
    bool empty = true;
};

[[nodiscard]] inline rbox make_box(float x, float y, float w, float h, const float (&radii)[4]) {
    rbox b{};
    if (!(w > 0.f) || !(h > 0.f))
        return b;
    b.hw = w * 0.5f;
    b.hh = h * 0.5f;
    b.cx = x + b.hw;
    b.cy = y + b.hh;
    const float limit = fmin(b.hw, b.hh);
    for (unsigned i = 0; i < 4; ++i)
        b.r[i] = fmin(fmax(radii[i], 0.f), limit);
    b.empty = false;
    return b;
}

[[nodiscard]] inline float pick_radius(const rbox& b, float dx, float dy) {
    return dx < 0.f ? (dy < 0.f ? b.r[0] : b.r[3]) : (dy < 0.f ? b.r[1] : b.r[2]);
}

/// GPUI's rounded-box signed distance (negative inside).
[[nodiscard]] inline float sdf(const rbox& b, float px, float py) {
    const float dx = px - b.cx;
    const float dy = py - b.cy;
    const float r = pick_radius(b, dx, dy);
    const float qx = std::fabs(dx) - b.hw + r;
    const float qy = std::fabs(dy) - b.hh + r;
    if (r == 0.f)
        return fmax(qx, qy);
    const float mx = fmax(qx, 0.f);
    const float my = fmax(qy, 0.f);
    // Off the corner arc one of the two is 0 and the length is the other — exactly what
    // the square root gives (√(x²) = |x| in IEEE binary arithmetic), without taking it.
    const float len = mx == 0.f ? my : (my == 0.f ? mx : std::sqrt(mx * mx + my * my));
    return len + fmin(fmax(qx, qy), 0.f) - r;
}

[[nodiscard]] inline float coverage(const rbox& b, float px, float py) {
    return b.empty ? 0.f : saturate(0.5f - sdf(b, px, py));
}

/// Inside this far from every edge (in the straight part), coverage is exactly 1 — ½ plus a
/// margin that covers the float rounding of `sdf`.
inline constexpr float k_interior_margin = 0.5f + 1.f / 32.f;

/// The pixels of row `py` whose centres are certainly fully inside `b`: [x0, x1).
inline void interior_span(const rbox& b, float py, int& x0, int& x1) {
    x0 = 0;
    x1 = 0;
    if (b.empty)
        return;
    const float dy = std::fabs(py - b.cy);
    if (dy > b.hh - k_interior_margin)
        return;
    const bool top = py < b.cy;
    const float rl = top ? b.r[0] : b.r[3];
    const float rr = top ? b.r[1] : b.r[2];
    const float left = b.cx - b.hw + k_interior_margin + (dy > b.hh - rl ? rl : 0.f);
    const float right = b.cx + b.hw - k_interior_margin - (dy > b.hh - rr ? rr : 0.f);
    if (right < left)
        return;
    x0 = clamp_px(std::ceil(left - 0.5f));
    x1 = clamp_px(std::floor(right - 0.5f)) + 1;
}

// ── quad ──

struct quad_setup {
    rbox outer{};
    rbox inner{};
    bool has_border = false;
    bool dashed = false;
    rgba bg0{};
    rgba bg1{};
    rgba border{};
    // Edges and widths for dashes (outer box).
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    float wt = 0.f, wr = 0.f, wb = 0.f, wl = 0.f;
    // Gradient.
    bool gradient = false;
    bool oklab = false;
    float dir_x = 1.f, dir_y = 0.f, inv_len = 0.f;
    float lab0[4]{}; ///< L, a, b, alpha
    float lab1[4]{};
};

inline void to_oklab(rgba premul, float (&out)[4]) {
    const float a = premul.a;
    const float inv = a > 0.f ? 1.f / a : 0.f;
    const float r = ui_math::srgb_decode(saturate(premul.r * inv));
    const float g = ui_math::srgb_decode(saturate(premul.g * inv));
    const float b = ui_math::srgb_decode(saturate(premul.b * inv));
    const float l = ui_math::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = ui_math::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = ui_math::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    out[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    out[3] = a;
}

[[nodiscard]] inline rgba from_oklab(const float (&lab)[4]) {
    const float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    const float r = saturate(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
    const float g = saturate(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
    const float b = saturate(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
    const float a = saturate(lab[3]);
    return {ui_math::srgb_encode(r) * a, ui_math::srgb_encode(g) * a, ui_math::srgb_encode(b) * a, a};
}

[[nodiscard]] inline quad_setup make_quad(const quad_instance& q) {
    quad_setup s{};
    float radii[4];
    float widths[4];
    for (unsigned i = 0; i < 4; ++i) {
        radii[i] = from_half(q.radii[i]);
        widths[i] = fmax(from_half(q.border_widths[i]), 0.f);
    }
    s.outer = make_box(q.bounds[0], q.bounds[1], q.bounds[2], q.bounds[3], radii);
    s.wt = widths[0];
    s.wr = widths[1];
    s.wb = widths[2];
    s.wl = widths[3];
    s.x0 = q.bounds[0];
    s.y0 = q.bounds[1];
    s.x1 = q.bounds[0] + q.bounds[2];
    s.y1 = q.bounds[1] + q.bounds[3];
    s.has_border = s.wt > 0.f || s.wr > 0.f || s.wb > 0.f || s.wl > 0.f;
    if (s.has_border) {
        const float* r = s.outer.r;
        const float inner_radii[4] = {fmax(r[0] - fmax(s.wt, s.wl), 0.f),
                                      fmax(r[1] - fmax(s.wt, s.wr), 0.f),
                                      fmax(r[2] - fmax(s.wb, s.wr), 0.f),
                                      fmax(r[3] - fmax(s.wb, s.wl), 0.f)};
        s.inner = make_box(s.x0 + s.wl, s.y0 + s.wt, q.bounds[2] - s.wl - s.wr,
                           q.bounds[3] - s.wt - s.wb, inner_radii);
    }
    s.dashed = s.has_border && q.border_style == 1;
    s.bg0 = half4(q.background);
    s.bg1 = half4(q.background_end);
    s.border = half4(q.border_color);
    const float angle = from_half(q.gradient_angle);
    s.gradient = std::memcmp(q.background, q.background_end, sizeof q.background) != 0;
    if (s.gradient) {
        const float theta = angle * (ui_math::k_pi / 180.f);
        s.dir_x = ui_math::cos(theta);
        s.dir_y = ui_math::sin(theta);
        const float len = std::fabs(q.bounds[2] * s.dir_x) + std::fabs(q.bounds[3] * s.dir_y);
        s.inv_len = len > 0.f ? 1.f / len : 0.f;
        s.oklab = q.gradient_space == 1;
        if (s.oklab) {
            to_oklab(s.bg0, s.lab0);
            to_oklab(s.bg1, s.lab1);
        }
    }
    return s;
}

[[nodiscard]] inline rgba background_at(const quad_setup& s, float px, float py) {
    if (!s.gradient)
        return s.bg0;
    const float t = saturate(0.5f + ((px - s.outer.cx) * s.dir_x + (py - s.outer.cy) * s.dir_y) *
                                        s.inv_len);
    if (!s.oklab)
        return add(s.bg0, scale(add(s.bg1, scale(s.bg0, -1.f)), t));
    const float lab[4] = {s.lab0[0] + (s.lab1[0] - s.lab0[0]) * t,
                          s.lab0[1] + (s.lab1[1] - s.lab0[1]) * t,
                          s.lab0[2] + (s.lab1[2] - s.lab0[2]) * t,
                          s.lab0[3] + (s.lab1[3] - s.lab0[3]) * t};
    return from_oklab(lab);
}

[[nodiscard]] inline float dash_coverage(const quad_setup& s, float px, float py) {
    const float* r = s.outer.r;
    const float tl_x = fmax(r[0], s.wl), tl_y = fmax(r[0], s.wt);
    const float tr_x = fmax(r[1], s.wr), tr_y = fmax(r[1], s.wt);
    const float br_x = fmax(r[2], s.wr), br_y = fmax(r[2], s.wb);
    const float bl_x = fmax(r[3], s.wl), bl_y = fmax(r[3], s.wb);
    if ((px < s.x0 + tl_x && py < s.y0 + tl_y) || (px >= s.x1 - tr_x && py < s.y0 + tr_y) ||
        (px >= s.x1 - br_x && py >= s.y1 - br_y) || (px < s.x0 + bl_x && py >= s.y1 - bl_y))
        return 1.f;
    // The edge whose band the pixel is least outside of.
    float best = 0.f;
    int edge = -1;
    const float excess[4] = {(py - s.y0) - s.wt, (s.x1 - px) - s.wr, (s.y1 - py) - s.wb,
                             (px - s.x0) - s.wl};
    const float widths[4] = {s.wt, s.wr, s.wb, s.wl};
    for (int e = 0; e < 4; ++e) {
        if (!(widths[e] > 0.f))
            continue;
        if (edge < 0 || excess[e] < best) {
            best = excess[e];
            edge = e;
        }
    }
    if (edge < 0)
        return 1.f;
    float t = 0.f;
    switch (edge) {
    case 0: t = px - (s.x0 + tl_x); break;
    case 1: t = py - (s.y0 + tr_y); break;
    case 2: t = (s.x1 - br_x) - px; break;
    default: t = (s.y1 - bl_y) - py; break;
    }
    const float w = widths[edge];
    const float period = 4.f * w;
    const float m = t - period * std::floor(t / period);
    const float on = m < 2.f * w ? fmin(m, 2.f * w - m) : -fmin(m - 2.f * w, period - m);
    return saturate(0.5f + on);
}

[[nodiscard, gnu::always_inline]] inline rgba shade_quad(const quad_setup& s, float px, float py) {
    const float a_o = coverage(s.outer, px, py);
    if (!(a_o > 0.f))
        return {};
    const rgba bg = background_at(s, px, py);
    if (!s.has_border)
        return scale(bg, a_o);
    const float a_i = coverage(s.inner, px, py);
    const float w_i = fmin(a_i, a_o);
    const float w_b = a_o - w_i;
    rgba out = scale(bg, w_i);
    if (w_b > 0.f) {
        const rgba border = s.dashed ? scale(s.border, dash_coverage(s, px, py)) : s.border;
        out = add(out, scale(over(border, bg), w_b));
    }
    return out;
}

// ── shadow ──

struct shadow_setup {
    rbox box{};      ///< outer: the blurred box; inset: the hole
    rbox element{};  ///< inset only
    float sigma = 0.f;
    bool inset = false;
    bool hard = false;
    rgba color{};
    float k = 0.f;   ///< √½ / σ
    float g_norm = 0.f;
    float g_exp = 0.f;
};

[[nodiscard]] inline shadow_setup make_shadow(const shadow_instance& sh) {
    shadow_setup s{};
    float radii[4];
    for (unsigned i = 0; i < 4; ++i)
        radii[i] = from_half(sh.radii[i]);
    s.inset = (sh.flags & 1u) != 0;
    s.sigma = fmax(sh.sigma, 0.f);
    s.hard = s.sigma < 1.e-4f;
    s.color = half4(sh.color);
    const float sp = sh.spread;
    if (s.inset) {
        s.element = make_box(sh.bounds[0], sh.bounds[1], sh.bounds[2], sh.bounds[3], radii);
        const float hole_radii[4] = {fmax(radii[0] - sp, 0.f), fmax(radii[1] - sp, 0.f),
                                     fmax(radii[2] - sp, 0.f), fmax(radii[3] - sp, 0.f)};
        s.box = make_box(sh.bounds[0] + sp, sh.bounds[1] + sp, sh.bounds[2] - 2.f * sp,
                         sh.bounds[3] - 2.f * sp, hole_radii);
    } else {
        const float grown[4] = {fmax(radii[0] + sp, 0.f), fmax(radii[1] + sp, 0.f),
                                fmax(radii[2] + sp, 0.f), fmax(radii[3] + sp, 0.f)};
        s.box = make_box(sh.bounds[0] - sp, sh.bounds[1] - sp, sh.bounds[2] + 2.f * sp,
                         sh.bounds[3] + 2.f * sp, grown);
    }
    if (!s.hard) {
        s.k = 0.70710678118f / s.sigma;
        s.g_norm = 1.f / (2.50662827463f * s.sigma);
        s.g_exp = -1.f / (2.f * s.sigma * s.sigma);
    }
    return s;
}

/// Evan Wallace's blurred rounded box at the pixel centre.
[[nodiscard]] inline float shadow_mask(const shadow_setup& s, float px, float py) {
    const rbox& b = s.box;
    if (b.empty)
        return 0.f;
    if (s.hard)
        return coverage(b, px, py);
    const float dx = px - b.cx;
    const float dy = py - b.cy;
    const float corner = pick_radius(b, dx, dy);
    const float three = 3.f * s.sigma;
    const float low = dy - b.hh;
    const float high = dy + b.hh;
    const float start = fmin(fmax(-three, low), high);
    const float end = fmin(fmax(three, low), high);
    const float step = (end - start) * 0.25f;
    float y = start + step * 0.5f;
    float value = 0.f;
    for (int i = 0; i < 4; ++i) {
        const float yy = dy - y;
        const float delta = fmin(b.hh - corner - std::fabs(yy), 0.f);
        const float curved = b.hw - corner + std::sqrt(fmax(0.f, corner * corner - delta * delta));
        const float lo = 0.5f + 0.5f * ui_math::erf((dx - curved) * s.k);
        const float hi = 0.5f + 0.5f * ui_math::erf((dx + curved) * s.k);
        const float gauss = ui_math::exp(y * y * s.g_exp) * s.g_norm;
        value = value + (hi - lo) * gauss * step;
        y = y + step;
    }
    return value;
}

[[nodiscard]] inline float shade_shadow(const shadow_setup& s, float px, float py) {
    if (!s.inset)
        return shadow_mask(s, px, py);
    const float element = coverage(s.element, px, py);
    if (!(element > 0.f))
        return 0.f;
    return element * (1.f - shadow_mask(s, px, py));
}

// ── sprites ──

/// Bilinear (or nearest) sample of one channel set at texel-space point (u, v) — the
/// pixel centre already mapped — clamped to the tile's texel centres.
struct tile_sampler {
    const std::uint8_t* pixels = nullptr;
    unsigned stride = 0;
    unsigned bpt = 1;
    int u0 = 0, v0 = 0, u1 = 0, v1 = 0; ///< tile, texels, exclusive end
};

inline void sample(const tile_sampler& t, float u, float v, float (&out)[4]) {
    float su = u - 0.5f;
    float sv = v - 0.5f;
    const float lo_u = static_cast<float>(t.u0);
    const float hi_u = static_cast<float>(t.u1 - 1);
    const float lo_v = static_cast<float>(t.v0);
    const float hi_v = static_cast<float>(t.v1 - 1);
    su = su < lo_u ? lo_u : (su > hi_u ? hi_u : su);
    sv = sv < lo_v ? lo_v : (sv > hi_v ? hi_v : sv);
    const float fu = std::floor(su);
    const float fv = std::floor(sv);
    const float wx = su - fu;
    const float wy = sv - fv;
    const int i0 = static_cast<int>(fu);
    const int j0 = static_cast<int>(fv);
    const int i1 = i0 + 1 < t.u1 ? i0 + 1 : i0;
    const int j1 = j0 + 1 < t.v1 ? j0 + 1 : j0;
    const std::uint8_t* r0 = t.pixels + static_cast<std::size_t>(j0) * t.stride;
    const std::uint8_t* r1 = t.pixels + static_cast<std::size_t>(j1) * t.stride;
    const float* un = unorm().v;
    for (unsigned c = 0; c < t.bpt; ++c) {
        const float p00 = un[r0[static_cast<std::size_t>(i0) * t.bpt + c]];
        const float p10 = un[r0[static_cast<std::size_t>(i1) * t.bpt + c]];
        const float p01 = un[r1[static_cast<std::size_t>(i0) * t.bpt + c]];
        const float p11 = un[r1[static_cast<std::size_t>(i1) * t.bpt + c]];
        const float top = p00 + (p10 - p00) * wx;
        const float bottom = p01 + (p11 - p01) * wx;
        out[c] = top + (bottom - top) * wy;
    }
}

inline void fetch(const tile_sampler& t, int i, int j, float (&out)[4]) {
    const std::uint8_t* p = t.pixels + static_cast<std::size_t>(j) * t.stride +
                            static_cast<std::size_t>(i) * t.bpt;
    const float* un = unorm().v;
    for (unsigned c = 0; c < t.bpt; ++c)
        out[c] = un[p[c]];
}

} // namespace lx::gfx::ui_raster

// ── What the GPU backends share (`:ui_gl`, `:ui_vk`) ────────────────────────────────
//
// Not exported: module-internal, visible to the partitions that import this one. The stream
// is encoded into a ring slot the same way by both, so the batches, their instance counts
// and every skip rule agree — and with them the pixels.

namespace lx::gfx::ui_encode {

/// Bytes per instance of a batch's kind in a ring slot; 0 for surfaces (they stay blit
/// commands).
[[nodiscard]] inline unsigned stride(primitive_kind kind) {
    switch (kind) {
    case primitive_kind::quad: return sizeof(quad_instance);
    case primitive_kind::shadow: return sizeof(shadow_instance);
    case primitive_kind::mono_sprite:
    case primitive_kind::poly_sprite:
    case primitive_kind::path: return sizeof(sprite_instance);
    case primitive_kind::underline: return sizeof(underline_instance);
    case primitive_kind::surface: return 0;
    }
    return 0;
}

[[nodiscard]] inline bool meets(lx::rect2i a, lx::rect2i b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

[[nodiscard]] inline lx::rect2i intersect(lx::rect2i a, lx::rect2i b) {
    const long long x0 = a.x > b.x ? a.x : b.x;
    const long long y0 = a.y > b.y ? a.y : b.y;
    const long long ax1 = static_cast<long long>(a.x) + a.width;
    const long long bx1 = static_cast<long long>(b.x) + b.width;
    const long long ay1 = static_cast<long long>(a.y) + a.height;
    const long long by1 = static_cast<long long>(b.y) + b.height;
    const long long x1 = ax1 < bx1 ? ax1 : bx1;
    const long long y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

/// Copies the batch's instances (refs `first`, `count`) of one kind into `out`, in sorted
/// order, a run of consecutive indices with one memcpy. `valid(index)` rejects an instance
/// (skipped and counted). Returns the instances written.
template <typename Instance, typename Valid>
unsigned gather(std::span<const primitive_ref> refs, std::uint32_t first, std::uint32_t count,
                std::span<const Instance> instances, std::byte* out, unsigned& skipped,
                Valid&& valid) {
    unsigned written = 0;
    std::size_t run_start = 0;
    std::size_t run_len = 0;
    const auto flush = [&]() {
        if (run_len == 0)
            return;
        std::memcpy(out + static_cast<std::size_t>(written) * sizeof(Instance),
                    instances.data() + run_start, run_len * sizeof(Instance));
        written += static_cast<unsigned>(run_len);
        run_len = 0;
    };
    const std::size_t end = static_cast<std::size_t>(first) + count;
    for (std::size_t i = first; i < end && i < refs.size(); ++i) {
        const std::uint32_t index = refs[i].index;
        if (index >= instances.size() || !valid(instances[index])) {
            ++skipped;
            continue;
        }
        if (run_len > 0 && index == run_start + run_len) {
            ++run_len;
            continue;
        }
        flush();
        run_start = index;
        run_len = 1;
    }
    flush();
    return written;
}

struct encode_counts {
    unsigned encoded = 0;        ///< instances written
    unsigned skipped = 0;        ///< bad index, missing or mismatched page, bad uv
    unsigned overflow_drops = 0; ///< primitives of batches that did not fit the slot
};

/// Encodes `stream` into one ring slot of `capacity` bytes: every batch's valid instances,
/// contiguous and in sorted order, the batches one after another. `batch_word[b]` gets the
/// batch's first word in the slot and `batch_count[b]` its instances (0: nothing to draw),
/// for the first `k_max_batches` batches; a batch that does not fit is dropped whole
/// (warn_and_drop: the caller sees the count). Sprite batches keep `:ui_cpu`'s rule: the page
/// must exist with the kind's format, each instance on the batch's page with its tile inside
/// it. Returns the bytes written.
[[nodiscard]] inline std::size_t encode(const ui_stream& stream, const atlas* a, std::byte* slot,
                                        std::size_t capacity, std::uint32_t* batch_word,
                                        std::uint32_t* batch_count, encode_counts& counts) {
    std::size_t cursor = 0;
    const std::size_t batches = stream.batches.size();
    for (std::size_t b = 0; b < batches && b < k_max_batches; ++b) {
        const batch& bt = stream.batches[b];
        batch_word[b] = 0;
        batch_count[b] = 0;
        const unsigned size = stride(bt.kind);
        if (size == 0)
            continue;
        const std::size_t need = static_cast<std::size_t>(bt.count) * size;
        if (cursor + need > capacity) {
            counts.overflow_drops += bt.count;
            continue;
        }
        std::byte* out = slot + cursor;
        unsigned& skipped = counts.skipped;
        unsigned n = 0;
        switch (bt.kind) {
        case primitive_kind::quad:
            n = gather(stream.refs, bt.first, bt.count, stream.quads, out, skipped,
                       [](const quad_instance&) { return true; });
            break;
        case primitive_kind::shadow:
            n = gather(stream.refs, bt.first, bt.count, stream.shadows, out, skipped,
                       [](const shadow_instance&) { return true; });
            break;
        case primitive_kind::underline:
            n = gather(stream.refs, bt.first, bt.count, stream.underlines, out, skipped,
                       [](const underline_instance&) { return true; });
            break;
        case primitive_kind::mono_sprite:
        case primitive_kind::poly_sprite:
        case primitive_kind::path: {
            const atlas_page_view page = a ? a->page(bt.page) : atlas_page_view{};
            const atlas_format want =
                bt.kind == primitive_kind::poly_sprite ? atlas_format::rgba8 : atlas_format::r8;
            if (!page.valid() || page.format != want || bt.page >= k_atlas_pages) {
                skipped += bt.count;
                break;
            }
            const unsigned page_size = page.size;
            const std::uint8_t pg = bt.page;
            n = gather(stream.refs, bt.first, bt.count, stream.sprites, out, skipped,
                       [page_size, pg](const sprite_instance& sp) {
                           return sp.page == pg && sp.uv[2] > sp.uv[0] && sp.uv[3] > sp.uv[1] &&
                                  sp.uv[2] <= page_size && sp.uv[3] <= page_size;
                       });
            break;
        }
        case primitive_kind::surface:
            break;
        }
        batch_word[b] = static_cast<std::uint32_t>(cursor / 4u);
        batch_count[b] = n;
        cursor += static_cast<std::size_t>(n) * size;
        counts.encoded += n;
    }
    for (std::size_t b = k_max_batches; b < batches; ++b)
        counts.overflow_drops += stream.batches[b].count;
    return cursor;
}

} // namespace lx::gfx::ui_encode

// ── ui_cpu_renderer ─────────────────────────────────────────────────────────────

namespace {

using namespace lx::gfx::ui_raster;

struct ui_cpu_frame {
    const lx::gfx::pixel_surface* target = nullptr;
    bool has_alpha = false;
    bool fast = true;
    lx::gfx::ui_draw_stats* stats = nullptr;

    [[nodiscard]] std::uint32_t* row(int y) const {
        return reinterpret_cast<std::uint32_t*>(target->pixels +
                                                static_cast<std::size_t>(y) * target->stride);
    }
};

void ui_cpu_quad(const ui_cpu_frame& f, const lx::gfx::quad_instance& q, lx::rect2i rect) {
    const window win = raster_window(q.bounds[0], q.bounds[1], q.bounds[2], q.bounds[3], q.clip, rect);
    if (win.empty())
        return;
    const quad_setup s = make_quad(q);
    if (s.outer.empty)
        return;
    // A solid opaque interior is one precomputed word: blending an opaque colour at full
    // coverage replaces the pixel with it, the same bytes `blend` would produce.
    const bool solid_word = !s.gradient && s.bg0.a >= 1.f;
    const std::uint32_t word = pack(s.bg0, f.has_alpha);
    for (int y = win.y0; y < win.y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        std::uint32_t* row = f.row(y);
        int fx0 = win.x0;
        int fx1 = win.x0;
        if (f.fast) {
            int ox0 = 0, ox1 = 0;
            interior_span(s.outer, py, ox0, ox1);
            if (s.has_border) {
                int ix0 = 0, ix1 = 0;
                interior_span(s.inner, py, ix0, ix1);
                ox0 = ix0 > ox0 ? ix0 : ox0;
                ox1 = ix1 < ox1 ? ix1 : ox1;
            }
            fx0 = ox0 > win.x0 ? ox0 : win.x0;
            fx1 = ox1 < win.x1 ? ox1 : win.x1;
            if (fx1 <= fx0)
                fx0 = fx1 = win.x1;
        }
        for (int x = win.x0; x < win.x1; ++x) {
            if (x == fx0 && fx1 > fx0) {
                for (; x < fx1; ++x) {
                    if (solid_word)
                        row[x] = word;
                    else
                        blend(row[x], background_at(s, static_cast<float>(x) + 0.5f, py),
                              f.has_alpha);
                }
                if (x >= win.x1)
                    break;
            }
            blend(row[x], shade_quad(s, static_cast<float>(x) + 0.5f, py), f.has_alpha);
        }
    }
    f.stats->pixels_shaded += static_cast<unsigned long long>(win.x1 - win.x0) *
                              static_cast<unsigned long long>(win.y1 - win.y0);
    ++f.stats->primitives_drawn;
}

void ui_cpu_shadow(const ui_cpu_frame& f, const lx::gfx::shadow_instance& sh, lx::rect2i rect) {
    const bool inset = (sh.flags & 1u) != 0;
    const float reach = inset ? 0.f : 3.f * fmax(sh.sigma, 0.f) + sh.spread;
    const window win = raster_window(sh.bounds[0] - reach, sh.bounds[1] - reach,
                                     sh.bounds[2] + 2.f * reach, sh.bounds[3] + 2.f * reach,
                                     sh.clip, rect);
    if (win.empty())
        return;
    const shadow_setup s = make_shadow(sh);
    for (int y = win.y0; y < win.y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        std::uint32_t* row = f.row(y);
        for (int x = win.x0; x < win.x1; ++x) {
            const float mask = shade_shadow(s, static_cast<float>(x) + 0.5f, py);
            if (mask > 0.f)
                blend(row[x], scale(s.color, mask), f.has_alpha);
        }
    }
    f.stats->pixels_shaded += static_cast<unsigned long long>(win.x1 - win.x0) *
                              static_cast<unsigned long long>(win.y1 - win.y0);
    ++f.stats->primitives_drawn;
}

void ui_cpu_sprite(const ui_cpu_frame& f, lx::gfx::primitive_kind kind,
                   const lx::gfx::sprite_instance& sp, const lx::gfx::atlas* atlas,
                   lx::rect2i rect) {
    using lx::gfx::atlas_format;
    const bool poly = kind == lx::gfx::primitive_kind::poly_sprite;
    const lx::gfx::atlas_page_view page = atlas ? atlas->page(sp.page) : lx::gfx::atlas_page_view{};
    const atlas_format want = poly ? atlas_format::rgba8 : atlas_format::r8;
    if (!page.valid() || page.format != want || sp.uv[2] <= sp.uv[0] || sp.uv[3] <= sp.uv[1] ||
        sp.uv[2] > page.size || sp.uv[3] > page.size) {
        ++f.stats->primitives_skipped;
        return;
    }
    const window win = raster_window(sp.bounds[0], sp.bounds[1], sp.bounds[2], sp.bounds[3], sp.clip, rect);
    if (win.empty())
        return;
    tile_sampler t{};
    t.pixels = page.pixels;
    t.stride = page.stride;
    t.bpt = poly ? 4u : 1u;
    t.u0 = sp.uv[0];
    t.v0 = sp.uv[1];
    t.u1 = sp.uv[2];
    t.v1 = sp.uv[3];
    const float tw = static_cast<float>(t.u1 - t.u0);
    const float th = static_cast<float>(t.v1 - t.v0);
    const float bx = sp.bounds[0];
    const float by = sp.bounds[1];
    // A turned sprite: its unturned size, and the basis mapping its pixels back into it.
    const unsigned xf = (sp.flags >> 1u) & 7u;
    const lx::gfx::source_basis basis = lx::gfx::source_basis_for(static_cast<lx::buffer_transform>(xf));
    const bool turned = xf != 0 && basis.dx[0] == 0.f;
    const float cw = turned ? sp.bounds[3] : sp.bounds[2];
    const float ch = turned ? sp.bounds[2] : sp.bounds[3];
    const float ku = tw / cw;
    const float kv = th / ch;
    const bool nearest = bx == std::floor(bx) && by == std::floor(by) && cw == tw && ch == th;
    const rgba color = half4(sp.color);
    const bool grayscale = poly && (sp.flags & 1u) != 0;
    rbox mask{};
    bool rounded = false;
    if (poly) {
        float radii[4];
        for (unsigned i = 0; i < 4; ++i)
            radii[i] = lx::gfx::from_half(sp.params[i]);
        rounded = radii[0] > 0.f || radii[1] > 0.f || radii[2] > 0.f || radii[3] > 0.f;
        mask = make_box(sp.bounds[0], sp.bounds[1], sp.bounds[2], sp.bounds[3], radii);
    }
    const int ox = clamp_px(bx);
    const int oy = clamp_px(by);
    for (int y = win.y0; y < win.y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        std::uint32_t* row = f.row(y);
        const float v = static_cast<float>(t.v0) + (py - by) * kv;
        for (int x = win.x0; x < win.x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            float texel[4]{};
            if (xf != 0) {
                // The pixel's centre in the unturned box: the basis is a signed permutation,
                // so whole-pixel offsets stay exact.
                const float dx = px - bx;
                const float dy = py - by;
                const float cx = basis.origin[0] * cw + basis.dx[0] * dx + basis.dy[0] * dy;
                const float cy = basis.origin[1] * ch + basis.dx[1] * dx + basis.dy[1] * dy;
                if (nearest)
                    fetch(t, t.u0 + static_cast<int>(std::floor(cx)),
                          t.v0 + static_cast<int>(std::floor(cy)), texel);
                else
                    sample(t, static_cast<float>(t.u0) + cx * ku, static_cast<float>(t.v0) + cy * kv,
                           texel);
            } else if (nearest)
                fetch(t, t.u0 + (x - ox), t.v0 + (y - oy), texel);
            else
                sample(t, static_cast<float>(t.u0) + (px - bx) * ku, v, texel);
            rgba src{};
            if (poly) {
                // R, G, B, A bytes, premultiplied.
                rgba c{texel[0], texel[1], texel[2], texel[3]};
                if (grayscale) {
                    const float l = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
                    c.r = c.g = c.b = l;
                }
                src = {c.r * color.r, c.g * color.g, c.b * color.b, c.a * color.a};
                if (rounded)
                    src = scale(src, coverage(mask, px, py));
            } else {
                src = scale(color, texel[0]);
            }
            blend(row[x], src, f.has_alpha);
        }
    }
    f.stats->pixels_shaded += static_cast<unsigned long long>(win.x1 - win.x0) *
                              static_cast<unsigned long long>(win.y1 - win.y0);
    ++f.stats->primitives_drawn;
}

void ui_cpu_underline(const ui_cpu_frame& f, const lx::gfx::underline_instance& u, lx::rect2i rect) {
    const window win = raster_window(u.bounds[0], u.bounds[1], u.bounds[2], u.bounds[3], u.clip, rect);
    if (win.empty())
        return;
    const float t = fmax(lx::gfx::from_half(u.thickness), 0.f);
    if (!(t > 0.f))
        return;
    const rgba color = half4(u.color);
    const float yc = u.bounds[1] + u.bounds[3] * 0.5f;
    const float half_t = t * 0.5f;
    const float omega = 2.f * lx::gfx::ui_math::k_pi / (4.f * t);
    for (int y = win.y0; y < win.y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        std::uint32_t* row = f.row(y);
        for (int x = win.x0; x < win.x1; ++x) {
            float dist = 0.f;
            if (u.wavy != 0) {
                const float phase = (static_cast<float>(x) + 0.5f - u.bounds[0]) * omega;
                const float fy = yc + t * lx::gfx::ui_math::sin(phase);
                const float slope = t * omega * lx::gfx::ui_math::cos(phase);
                dist = std::fabs(py - fy) / std::sqrt(1.f + slope * slope);
            } else {
                dist = std::fabs(py - yc);
            }
            const float cov = saturate(half_t - dist + 0.5f);
            if (cov > 0.f)
                blend(row[x], scale(color, cov), f.has_alpha);
        }
    }
    f.stats->pixels_shaded += static_cast<unsigned long long>(win.x1 - win.x0) *
                              static_cast<unsigned long long>(win.y1 - win.y0);
    ++f.stats->primitives_drawn;
}

[[nodiscard]] bool lx_gfx_ui_meets(lx::rect2i a, lx::rect2i b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

/// The quad writes one opaque colour over every pixel of `rect`: square corners, no border,
/// a solid opaque background, whole-pixel bounds and content mask containing the rect (so
/// every centre in it has coverage exactly 1). Clearing under it changes nothing.
[[nodiscard]] bool lx_gfx_ui_covers(const lx::gfx::quad_instance& q, lx::rect2i rect) {
    for (unsigned i = 0; i < 4; ++i)
        if (q.radii[i] != 0 || q.border_widths[i] != 0)
            return false;
    if (std::memcmp(q.background, q.background_end, sizeof q.background) != 0 ||
        lx::gfx::from_half(q.background[3]) < 1.f)
        return false;
    const auto whole = [](const float (&r)[4]) {
        for (const float v : r)
            if (v != std::floor(v) || !(v > -1.e6f && v < 1.e6f))
                return false;
        return true;
    };
    const auto contains = [&](const float (&r)[4]) {
        return r[0] <= static_cast<float>(rect.x) && r[1] <= static_cast<float>(rect.y) &&
               r[0] + r[2] >= static_cast<float>(rect.x + rect.width) &&
               r[1] + r[3] >= static_cast<float>(rect.y + rect.height);
    };
    if (!whole(q.bounds) || !contains(q.bounds))
        return false;
    const bool masked = q.clip[2] > 0.f && q.clip[3] > 0.f;
    return !masked || (whole(q.clip) && contains(q.clip));
}

} // namespace

lx::gfx::ui_cpu_renderer::ui_cpu_renderer()
    : rows_{std::make_unique<std::int32_t[]>(2u * k_ui_cpu_max_refs)} {}

namespace {

/// The rows [y0, y1) a ref can write: its raster bounds and content mask (or, for a
/// surface, its clipped destination), within the target.
void lx_gfx_ui_ref_rows(const lx::gfx::ui_stream& st, const lx::gfx::primitive_ref& ref,
                        lx::rect2i full, std::int32_t& y0, std::int32_t& y1) {
    using lx::gfx::primitive_kind;
    window w{};
    switch (ref.kind) {
    case primitive_kind::quad:
        if (ref.index < st.quads.size()) {
            const auto& q = st.quads[ref.index];
            w = raster_window(q.bounds[0], q.bounds[1], q.bounds[2], q.bounds[3], q.clip, full);
        }
        break;
    case primitive_kind::shadow:
        if (ref.index < st.shadows.size()) {
            const auto& sh = st.shadows[ref.index];
            const float reach = (sh.flags & 1u) != 0 ? 0.f : 3.f * fmax(sh.sigma, 0.f) + sh.spread;
            w = raster_window(sh.bounds[0] - reach, sh.bounds[1] - reach, sh.bounds[2] + 2.f * reach,
                              sh.bounds[3] + 2.f * reach, sh.clip, full);
        }
        break;
    case primitive_kind::mono_sprite:
    case primitive_kind::poly_sprite:
    case primitive_kind::path:
        if (ref.index < st.sprites.size()) {
            const auto& sp = st.sprites[ref.index];
            w = raster_window(sp.bounds[0], sp.bounds[1], sp.bounds[2], sp.bounds[3], sp.clip, full);
        }
        break;
    case primitive_kind::underline:
        if (ref.index < st.underlines.size()) {
            const auto& u = st.underlines[ref.index];
            w = raster_window(u.bounds[0], u.bounds[1], u.bounds[2], u.bounds[3], u.clip, full);
        }
        break;
    case primitive_kind::surface:
        if (ref.index < st.surfaces.size()) {
            const lx::gfx::blit_command& c = st.surfaces[ref.index];
            lx::rect2i r = lx::gfx::detail::intersect(lx::gfx::detail::clipped_dst(c.dst, c.clip), full);
            w = {r.x, r.y, r.x + r.width, r.y + r.height};
        }
        break;
    }
    if (w.empty()) {
        y0 = 0;
        y1 = 0;
        return;
    }
    y0 = w.y0;
    y1 = w.y1;
}

} // namespace

lx::result<lx::gfx::ui_draw_stats> lx::gfx::ui_cpu_renderer::draw(const pixel_surface& target,
                                                                const ui_stream& stream,
                                                                const atlas* atlas,
                                                                const damage_region& damage,
                                                                std::optional<lx::color> clear) {
    const auto argb = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    const auto xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    if (!target.valid())
        return lx::invalid_argument("ui_cpu_renderer::draw: invalid target");
    if (target.format != argb && target.format != xrgb)
        return lx::invalid_argument("ui_cpu_renderer::draw: target must be XRGB8888 or ARGB8888");

    ui_draw_stats stats{};
    ui_cpu_frame f{};
    f.target = &target;
    f.has_alpha = target.format == argb;
    f.fast = fast_paths_;
    f.stats = &stats;

    const lx::rect2i full{0, 0, static_cast<int>(target.width), static_cast<int>(target.height)};
    lx::rect2i rects[k_damage_rects]{};
    unsigned rect_count = 0;
    if (damage.full) {
        rects[rect_count++] = full;
    } else {
        for (unsigned i = 0; i < damage.count && i < k_damage_rects; ++i) {
            const lx::rect2i r = detail::intersect(damage.rects[i], full);
            if (r.width > 0 && r.height > 0)
                rects[rect_count++] = r;
        }
    }
    if (rect_count == 0)
        return stats;

    // Row extents of every ref, once per draw: what lets a band skip what misses it.
    const bool banded = stream.refs.size() <= k_ui_cpu_max_refs;
    if (banded)
        for (std::size_t i = 0; i < stream.refs.size(); ++i)
            lx_gfx_ui_ref_rows(stream, stream.refs[i], full, rows_[2 * i], rows_[2 * i + 1]);

    std::optional<std::uint32_t> clear_word{};
    if (clear)
        clear_word = pack({clear->r, clear->g, clear->b, clear->a}, f.has_alpha);
    for (unsigned n = 0; n < rect_count; ++n) {
        const lx::rect2i rect = rects[n];
        ++stats.rects;
        int band_rows = rect.height;
        if (banded) {
            band_rows = static_cast<int>(k_ui_cpu_band_pixels) / rect.width;
            band_rows = band_rows < 8 ? 8 : band_rows;
            band_rows = band_rows > rect.height ? rect.height : band_rows;
        }
        for (int y = rect.y; y < rect.y + rect.height; y += band_rows) {
            const int rows = rect.y + rect.height - y < band_rows ? rect.y + rect.height - y : band_rows;
            if (auto ok = draw_band(&f, stream, atlas, {rect.x, y, rect.width, rows}, banded, clear_word);
                !ok)
                return ok.get_error();
        }
    }
    return stats;
}

lx::result<void> lx::gfx::ui_cpu_renderer::draw_band(const void* frame, const ui_stream& stream,
                                                     const atlas* atlas, lx::rect2i band,
                                                     bool banded,
                                                     std::optional<std::uint32_t> clear_word) {
    const ui_cpu_frame& f = *static_cast<const ui_cpu_frame*>(frame);
    ui_draw_stats& stats = *f.stats;
    const auto in_band = [&](std::size_t i) {
        return !banded || (rows_[2 * i] < band.y + band.height && rows_[2 * i + 1] > band.y);
    };
    // The topmost ref that repaints the whole band opaquely (a window over it, a window's
    // background) hides everything under it, the clear included: drawing starts there —
    // the same pixels, the writes of what shows only (`cpu_compositor::composite`'s cull).
    std::size_t start = 0;
    bool covered = false;
    for (std::size_t b = stream.batches.size(); b-- > 0 && !covered;) {
        const batch& bt = stream.batches[b];
        if (!lx_gfx_ui_meets(bt.bounds, band))
            continue;
        if (bt.kind != primitive_kind::quad && bt.kind != primitive_kind::surface)
            continue;
        const std::size_t end = static_cast<std::size_t>(bt.first) + bt.count;
        for (std::size_t i = end < stream.refs.size() ? end : stream.refs.size(); i-- > bt.first;) {
            if (!in_band(i))
                continue;
            const primitive_ref& r = stream.refs[i];
            const bool cover =
                r.kind == primitive_kind::quad
                    ? r.index < stream.quads.size() && lx_gfx_ui_covers(stream.quads[r.index], band)
                    : compositor_ && r.index < stream.surfaces.size() &&
                          compositor_->covers_opaquely(stream.surfaces[r.index], f.target->format, band);
            if (cover) {
                start = i;
                covered = true;
                break;
            }
        }
    }
    if (clear_word && !covered) {
        for (int y = band.y; y < band.y + band.height; ++y) {
            std::uint32_t* row = f.row(y);
            for (int x = band.x; x < band.x + band.width; ++x)
                row[x] = *clear_word;
        }
    }
    for (const batch& b : stream.batches) {
        const std::size_t end = static_cast<std::size_t>(b.first) + b.count;
        // Missing the band, or entirely under the covering ref.
        if (!lx_gfx_ui_meets(b.bounds, band) || end <= start) {
            ++stats.batches_skipped;
            continue;
        }
        ++stats.batches_drawn;
        for (std::size_t i = b.first > start ? b.first : start; i < end && i < stream.refs.size(); ++i) {
            if (!in_band(i))
                continue;
            const primitive_ref& ref = stream.refs[i];
            switch (ref.kind) {
            case primitive_kind::quad:
                if (ref.index < stream.quads.size())
                    ui_cpu_quad(f, stream.quads[ref.index], band);
                else
                    ++stats.primitives_skipped;
                break;
            case primitive_kind::shadow:
                if (ref.index < stream.shadows.size())
                    ui_cpu_shadow(f, stream.shadows[ref.index], band);
                else
                    ++stats.primitives_skipped;
                break;
            case primitive_kind::mono_sprite:
            case primitive_kind::poly_sprite:
            case primitive_kind::path:
                if (ref.index < stream.sprites.size())
                    ui_cpu_sprite(f, ref.kind, stream.sprites[ref.index], atlas, band);
                else
                    ++stats.primitives_skipped;
                break;
            case primitive_kind::underline:
                if (ref.index < stream.underlines.size())
                    ui_cpu_underline(f, stream.underlines[ref.index], band);
                else
                    ++stats.primitives_skipped;
                break;
            case primitive_kind::surface:
                if (compositor_ && ref.index < stream.surfaces.size()) {
                    // A run of refs naming consecutive draws (the compositor's windows, in
                    // paint order) is one blend call: the same pixels, one setup.
                    std::size_t n = 1;
                    while (i + n < end && i + n < stream.refs.size() && in_band(i + n) &&
                           stream.refs[i + n].kind == primitive_kind::surface &&
                           stream.refs[i + n].index == ref.index + n &&
                           ref.index + n < stream.surfaces.size())
                        ++n;
                    auto drawn = compositor_->blend(*f.target, &stream.surfaces[ref.index],
                                                    static_cast<unsigned>(n), band);
                    if (!drawn)
                        return drawn.get_error();
                    stats.primitives_drawn += static_cast<unsigned>(n);
                    stats.surface_draws_skipped += drawn->draws_skipped;
                    stats.surface_draws_unsupported += drawn->draws_unsupported;
                    stats.surface_draws_culled += drawn->draws_culled;
                    i += n - 1;
                } else {
                    ++stats.primitives_skipped;
                }
                break;
            }
        }
    }
    return {};
}

// ── Lowering to blits ───────────────────────────────────────────────────────────

bool lx::gfx::quad_lowers_to_blit(const quad_instance& q) {
    for (unsigned k = 0; k < 4; ++k)
        if ((q.radii[k] & 0x7FFFu) != 0)
            return false;
    bool borderless = true;
    for (unsigned k = 0; k < 4; ++k)
        borderless = borderless && (q.border_widths[k] & 0x7FFFu) == 0;
    if (!borderless) {
        bool clear = true;
        for (unsigned k = 0; k < 4; ++k)
            clear = clear && (q.border_color[k] & 0x7FFFu) == 0;
        if (!clear)
            return false;
    }
    for (unsigned k = 0; k < 4; ++k)
        if (q.background[k] != q.background_end[k])
            return false;
    const auto whole = [](float v) { return v == std::floor(v) && v > -1e9f && v < 1e9f; };
    for (unsigned k = 0; k < 4; ++k)
        if (!whole(q.bounds[k]) || !whole(q.clip[k]))
            return false;
    return q.bounds[2] > 0.f && q.bounds[3] > 0.f;
}

unsigned lx::gfx::lower_to_blits(const ui_stream& stream, blit_command* out, unsigned capacity,
                                 ui_lower_stats& stats) {
    unsigned n = 0;
    const auto put = [&](const blit_command& c) {
        if (n < capacity && out) {
            out[n++] = c;
            return true;
        }
        ++stats.overflow;
        return false;
    };
    for (const primitive_ref& ref : stream.refs) {
        switch (ref.kind) {
        case primitive_kind::surface:
            if (ref.index < stream.surfaces.size() && put(stream.surfaces[ref.index]))
                ++stats.surfaces;
            break;
        case primitive_kind::quad: {
            if (ref.index >= stream.quads.size() || !quad_lowers_to_blit(stream.quads[ref.index])) {
                ++stats.dropped;
                break;
            }
            const quad_instance& q = stream.quads[ref.index];
            blit_command c{};
            c.solid = true;
            c.solid_color = lx::color::rgb(from_half(q.background[0]), from_half(q.background[1]),
                                           from_half(q.background[2]), from_half(q.background[3]));
            c.blend = c.solid_color.a >= 1.f ? lx::blend_mode::opaque : lx::blend_mode::premultiplied;
            c.dst = {static_cast<int>(q.bounds[0]), static_cast<int>(q.bounds[1]),
                     static_cast<int>(q.bounds[2]), static_cast<int>(q.bounds[3])};
            if (q.clip[2] > 0.f && q.clip[3] > 0.f)
                c.clip = {static_cast<int>(q.clip[0]), static_cast<int>(q.clip[1]),
                          static_cast<int>(q.clip[2]), static_cast<int>(q.clip[3])};
            if (put(c))
                ++stats.lowered;
            break;
        }
        default:
            ++stats.dropped;
            break;
        }
    }
    return n;
}
