module;

import lx.foundation;

export module lx.gfx:renderer;

import std;
import :headless;
import :damage;
import :color;

export namespace lx::gfx {

/// Disjoint rects a partly covered draw is confined to (`blit_command::visible`).
inline constexpr unsigned k_max_visible_rects = 4;

/// One quad handed to a present backend.
///
/// This mirrors `scene::draw_command`. It used to carry four of its thirteen fields, and
/// the six dropped in `tick_render` were exactly the ones `viewporter`,
/// `wp-fractional-scale-v1`, `alpha-modifier-v1`, subsurface clipping and rotated outputs
/// need — so those protocols could be parsed but never reach a pixel. Fields a backend
/// cannot express are reported through `composite_stats::draws_unsupported` rather than
/// ignored, so "not implemented here" stays visible instead of rendering the wrong frame.
struct blit_command {
    unsigned texture_id = 0;
    /// Destination rectangle in target pixels.
    lx::rect2i dst{};
    /// Source box in texture pixels, before `buffer_xform`; fractional edges are a
    /// `wp_viewport` crop. Empty means the whole texture — which is both a plain surface
    /// and a `wp_viewport` with no source set. Together with `dst` this expresses
    /// viewporter cropping and fractional scaling: the backends derive the scale factor
    /// from the src:dst ratio rather than being told it separately.
    lx::rect2f src{};
    /// Additional clip in target space; the draw is confined to its intersection with
    /// `dst`. Empty means unclipped. Subsurface clipping and layer-shell regions.
    lx::rect2i clip{};
    /// Per-draw color multiplier, applied premultiplied. `alpha-modifier-v1`.
    lx::color tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    float opacity = 1.f;
    lx::blend_mode blend = lx::blend_mode::premultiplied;
    /// `wl_output.transform` for the source buffer — rotated and flipped monitors.
    lx::buffer_transform buffer_xform = lx::buffer_transform::normal;
    /// Primaries the texture's color is expressed in.
    lx::color_space src_space = lx::color_space::srgb;
    /// Transfer function the texture is encoded with. The composite decodes to linear
    /// light before blending, because alpha blending is a weighted average of light and
    /// averaging encoded values darkens edges.
    lx::transfer_function src_transfer = lx::transfer_function::srgb;
    /// wp_color_manager_v1: how the source's colour maps into the composite's working space
    /// (`color_transform`, "Colour management" in color.cppm). Inactive — the default, and
    /// every sRGB surface — is the ordinary path above: the same shaders and pixels.
    color_transform color{};
    /// The draw resamples (`dst` is not the transformed source size, or the crop is off
    /// whole texels). A 1:1 draw on whole pixels samples texel centres exactly.
    bool scaled = false;
    /// Covered by an opaque draw above it: skipped (identical pixels).
    bool occluded = false;
    /// The part the opaque draws above leave visible, when they cover some of it: up to
    /// `k_max_visible_rects` disjoint rects in target pixels, inside the clipped `dst`
    /// (`scene::draw_command::visible`). Zero: the whole draw. A backend draws only inside
    /// them, as one more scissor — the same pixels, less fill.
    lx::rect2i visible[k_max_visible_rects]{};
    unsigned visible_count = 0;
    /// A solid colour instead of a texture (`wp_single_pixel_buffer_v1`): `texture_id` is 0,
    /// `src` is ignored and every pixel of `dst` is `solid_color` — premultiplied and
    /// encoded like a texel, then tinted, faded and blended like one. No texture exists and
    /// nothing is uploaded; an opaque, untinted one is a scissored clear where the backend
    /// has one.
    bool solid = false;
    lx::color solid_color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    /// A backdrop blur (ext-background-effect-v1): no texture; `dst` (inside `clip` and
    /// `visible`, as any draw) shows what the draws before it put in the target, blurred
    /// (`blur_planner`, "Backdrop blur" in rendering-performance.md), faded by opacity × tint
    /// alpha over what is there. `blur_box` is the bounds of the surface's blur region in
    /// target pixels, the same for every rect of it; `blur_source` numbers the frame's
    /// regions in draw order. Drawn only by a backend that `supports_blur` while a blur
    /// frame is open (`begin_blur_frame`); anywhere else it is skipped as culled.
    bool blur = false;
    unsigned blur_source = 0;
    lx::rect2i blur_box{};
};

/// A draw whose effective alpha (opacity × tint alpha) is zero changes no pixel: every
/// backend skips it. It still counts for the visibility pass (a surface fading in from 0
/// keeps its frame callbacks), which runs on the scene's draw list, not here.
[[nodiscard]] inline bool draw_invisible(const blit_command& cmd) {
    return !(cmd.opacity * cmd.tint.a > 0.f);
}

// ── Backdrop blur (ext-background-effect-v1) ───────────────────────────────────
//
// A dual-Kawase blur (Bjørge, "Bandwidth-efficient rendering", SIGGRAPH 2015; scenefx's
// blur passes) run at half resolution. For a blur region with bounds B on a target:
//
//   footprint F   B grown by `k_blur_margin` on every side, clipped to the target, its
//                 origin and size even (`blur_footprint`). Everything the blurred pixels
//                 of B depend on lies inside F.
//   level 0       F at half resolution: texel (i, j) is the target sampled bilinearly at
//                 F's pixel corner (2i + 1, 2j + 1) — the mean of that 2×2 block.
//   down 1..n     level k from level k−1: centre c = 2·(i + ½, j + ½) in level k−1
//                 texels, (4·S(c) + S(c ± (r, r)) + S(c ± (r, −r))) / 8.
//   up n..1       level k−1 from level k: c = (i + ½, j + ½) / 2 in level k texels, h = r/2,
//                 (S(c ± (2h, 0)) + S(c ± (0, 2h)) + 2·S(c ± (h, h)) + 2·S(c ± (h, −h))) / 12.
//   draw          each pixel of the region samples level 0 bilinearly at its centre.
//
// S is a bilinear sample clamped to the level's texel centres (clamp to edge), `n` =
// `k_blur_passes`, `r` = `k_blur_radius`. Every level has `blur_level_size` texels. With
// n = 3 and r = 1 the result is a blur of σ ≈ 15 output pixels whose support (29 level-0
// texels, 60 output pixels) lies inside the 64 pixel margin, so the pixels of B never see
// F's edge except where F meets the target's.

inline constexpr unsigned k_blur_passes = 3;
inline constexpr float k_blur_radius = 1.f;
inline constexpr int k_blur_margin = 64;
inline constexpr unsigned k_blur_levels = k_blur_passes + 1;
/// Blur regions one output frame may hold; more are not blurred (drawn as culled).
inline constexpr unsigned k_max_blur_sources = 8;

/// The footprint of a blur region with bounds `box` on a `width` × `height` target (see
/// above); empty when nothing of it is on the target.
[[nodiscard]] constexpr lx::rect2i blur_footprint(lx::rect2i box, int width, int height) {
    if (box.width <= 0 || box.height <= 0 || width < 2 || height < 2)
        return {};
    long long x0 = static_cast<long long>(box.x) - k_blur_margin;
    long long y0 = static_cast<long long>(box.y) - k_blur_margin;
    long long x1 = static_cast<long long>(box.x) + box.width + k_blur_margin;
    long long y1 = static_cast<long long>(box.y) + box.height + k_blur_margin;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > width ? width : x1;
    y1 = y1 > height ? height : y1;
    x0 &= ~1ll;
    y0 &= ~1ll;
    if (x1 - x0 < 2 || y1 - y0 < 2)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>((x1 - x0) & ~1ll),
            static_cast<int>((y1 - y0) & ~1ll)};
}

/// Texels of blur level `level` (0: half the footprint) for footprint `footprint`.
[[nodiscard]] constexpr lx::size2i blur_level_size(lx::rect2i footprint, unsigned level) {
    int w = footprint.width / 2;
    int h = footprint.height / 2;
    for (unsigned k = 0; k < level; ++k) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    return {w, h};
}

/// One blur region of an output's frame: its bounds in target pixels (`blit_command::
/// blur_box`), whether any of its draws reaches the screen (not occluded), and a signature
/// of what is painted under its footprint before it (`scene::blur_backdrop_signature`).
struct blur_source {
    lx::rect2i box{};
    bool visible = false;
    std::uint64_t backdrop = 0;
};

/// What an output knows about the blurs it last drew (render affinity; one per output in
/// each backend). A blurred level 0 is kept per region and drawn again, without blurring,
/// as long as the content behind it is the same (umbriel's cached background blur): its
/// backdrop signature — every draw and primitive painted under the footprint before the
/// region, with its content serial — and its bounds unchanged. The region's own surface,
/// and whatever is above it, may change freely: a panel redrawing its clock draws the
/// cached blur under the new pixels.
///
/// A stale region whose draws reach the screen is blurred again in the frame that draws it,
/// from the target itself, at the point of the blur draw: `plan` adds its footprint to the
/// frame's damage and gives it a rect of its own (`isolate`), so the composite redraws the
/// whole footprint in a single pass and every draw before the blur draw is in place when it
/// is read. Rects away from the footprint stay as they are: a text row of a window under a
/// blurred panel is still a row, beside the footprint. The blurred pixels changed, so the
/// damage reported for the frame covers them. A backend blurs a stale region only in a pass
/// whose rect holds the whole footprint, and culls its draw in any other.
class blur_planner {
public:
    /// Before compositing a frame whose draws hold blur draws: `sources` in `blur_source`
    /// order (at most `k_max_blur_sources` used), the frame's `damage` (what changed since
    /// the previous frame of this output), the target's size. Returns whether a region is
    /// blurred again in this frame — `damage` then holds its footprint, isolated.
    [[nodiscard]] bool plan(std::span<const blur_source> sources, damage_region& damage, int width,
                            int height);
    /// The region to composite (the damage of `plan`, with whatever else is folded in — the
    /// buffer age): each footprint blurred again in this frame and every rect meeting it
    /// become their bounds, repeated until that rect meets no other. Other rects are kept.
    void isolate(damage_region& region) const;
    [[nodiscard]] unsigned count() const { return count_; }
    [[nodiscard]] lx::rect2i footprint(unsigned k) const {
        return k < count_ ? entries_[k].footprint : lx::rect2i{};
    }
    /// Region `k`'s footprint where a draw naming `blur_box` meets it: the planned one, moved
    /// as far as the draw's box was (a screen capture's crop moves every draw of a frame).
    [[nodiscard]] lx::rect2i footprint_for(unsigned k, lx::rect2i blur_box) const {
        if (k >= count_)
            return {};
        lx::rect2i f = entries_[k].footprint;
        f.x += blur_box.x - entries_[k].box.x;
        f.y += blur_box.y - entries_[k].box.y;
        return f;
    }
    /// Region `k` is blurred when it is drawn in this frame (it is stale).
    [[nodiscard]] bool pending(unsigned k) const { return k < count_ && entries_[k].pending; }
    /// The backend blurred region `k` into its level 0: drawn from that, until it is stale.
    void done(unsigned k) {
        if (k < count_) {
            entries_[k].pending = false;
            entries_[k].cached = true;
        }
    }
    /// Nothing is cached any more (the backend dropped its levels).
    void reset();

private:
    struct entry {
        lx::rect2i box{};
        lx::rect2i footprint{};
        std::uint64_t backdrop = 0;
        bool cached = false;
        bool pending = false;
        /// Pending and drawn in this frame: its footprint is blurred again (`isolate`).
        bool again = false;
    };
    entry entries_[k_max_blur_sources]{};
    unsigned count_ = 0;
};

/// Where a draw's top-left, +x and +y directions land in its source box, in the box's own
/// normalized coordinates (0..1 across the box): the source point of normalized
/// destination (u, v) is `origin + u·dx + v·dy`. `transform` is the buffer transform
/// (wl_surface.set_buffer_transform): the compositor shows the buffer turned by its
/// inverse, as wlroots' `wlr_box_transform(…, invert(t), …)` maps a surface box into the
/// buffer. Shared by every backend so the eight transforms cannot disagree.
struct source_basis {
    float origin[2]{0.f, 0.f};
    float dx[2]{1.f, 0.f};
    float dy[2]{0.f, 1.f};
};

[[nodiscard]] constexpr source_basis source_basis_for(lx::buffer_transform transform) {
    // Indexed by the transform; each row is the inverse transform's point map.
    switch (transform) {
    case lx::buffer_transform::normal:
        return {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}};
    case lx::buffer_transform::rotate_90: // shown turned by 270: (v, 1 − u)
        return {{0.f, 1.f}, {0.f, -1.f}, {1.f, 0.f}};
    case lx::buffer_transform::rotate_180:
        return {{1.f, 1.f}, {-1.f, 0.f}, {0.f, -1.f}};
    case lx::buffer_transform::rotate_270: // shown turned by 90: (1 − v, u)
        return {{1.f, 0.f}, {0.f, 1.f}, {-1.f, 0.f}};
    case lx::buffer_transform::flipped:
        return {{1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}};
    case lx::buffer_transform::flipped_90:
        return {{0.f, 0.f}, {0.f, 1.f}, {1.f, 0.f}};
    case lx::buffer_transform::flipped_180:
        return {{0.f, 1.f}, {1.f, 0.f}, {0.f, -1.f}};
    case lx::buffer_transform::flipped_270:
        return {{1.f, 1.f}, {0.f, -1.f}, {-1.f, 0.f}};
    }
    return {};
}

/// Output transforms (wl_output.transform, ADR 008 §C.4). An output with transform `t`
/// scans out its content turned the way a buffer with buffer transform `t` holds its
/// surface's content — so a client that renders with the output's transform hands over a
/// buffer the output can show as it is (the reason wl_output.transform exists). Both maps
/// are `source_basis_for(t)`: content → buffer, content → output pixels.
///
/// The draw transform that shows a buffer of transform `buffer` on an output of transform
/// `output`: the content of the output's pixel p is `R_output⁻¹(p)`, and that content is
/// the buffer's `R_buffer(·)`, so the draw samples through `R_buffer ∘ R_output⁻¹` (wlroots'
/// `compose(invert(buffer), output)` in its own convention).
[[nodiscard]] constexpr lx::buffer_transform compose_output_transform(lx::buffer_transform buffer,
                                                                      lx::buffer_transform output) {
    // Each transform as the 2×2 matrix of its basis on centred coordinates (columns dx, dy);
    // all eight are signed permutations, so integer arithmetic is exact.
    const auto mat = [](lx::buffer_transform t, int (&m)[4]) {
        const source_basis b = source_basis_for(t);
        m[0] = static_cast<int>(b.dx[0]);
        m[1] = static_cast<int>(b.dy[0]);
        m[2] = static_cast<int>(b.dx[1]);
        m[3] = static_cast<int>(b.dy[1]);
    };
    int mb[4]{}, mo[4]{};
    mat(buffer, mb);
    mat(output, mo);
    // R_output⁻¹ is its transpose (orthogonal).
    const int inv[4] = {mo[0], mo[2], mo[1], mo[3]};
    const int want[4] = {mb[0] * inv[0] + mb[1] * inv[2], mb[0] * inv[1] + mb[1] * inv[3],
                         mb[2] * inv[0] + mb[3] * inv[2], mb[2] * inv[1] + mb[3] * inv[3]};
    for (unsigned i = 0; i < 8; ++i) {
        int m[4]{};
        mat(static_cast<lx::buffer_transform>(i), m);
        if (m[0] == want[0] && m[1] == want[1] && m[2] == want[2] && m[3] == want[3])
            return static_cast<lx::buffer_transform>(i);
    }
    return lx::buffer_transform::normal;
}

/// Where rect `r` of a `width` × `height` content space lands in the pixels of an output of
/// transform `t` (whose pixel space is `height` × `width` for the 90° ones): the same map,
/// exact on whole pixels. An empty rect stays empty.
[[nodiscard]] constexpr lx::rect2i transform_rect(lx::rect2i r, lx::buffer_transform t, int width,
                                                  int height) {
    if (t == lx::buffer_transform::normal || r.width <= 0 || r.height <= 0)
        return r;
    const source_basis b = source_basis_for(t);
    const bool turned = b.dx[0] == 0.f;
    const long long fw = turned ? height : width;
    const long long fh = turned ? width : height;
    // A content point (x, y) → (ox·fw + dx.x·x + dy.x·y, oy·fh + dx.y·x + dy.y·y): each
    // non-zero coefficient pairs the coordinate with the axis of the same length.
    const auto map = [&](long long x, long long y, long long& ox, long long& oy) {
        ox = static_cast<long long>(b.origin[0]) * fw + static_cast<long long>(b.dx[0]) * x +
             static_cast<long long>(b.dy[0]) * y;
        oy = static_cast<long long>(b.origin[1]) * fh + static_cast<long long>(b.dx[1]) * x +
             static_cast<long long>(b.dy[1]) * y;
    };
    long long ax = 0, ay = 0, bx = 0, by = 0;
    map(r.x, r.y, ax, ay);
    map(static_cast<long long>(r.x) + r.width, static_cast<long long>(r.y) + r.height, bx, by);
    const long long x0 = ax < bx ? ax : bx;
    const long long y0 = ay < by ? ay : by;
    const long long x1 = ax < bx ? bx : ax;
    const long long y1 = ay < by ? by : ay;
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

/// The source box a draw samples, in texture pixels: `src`, or the whole texture when it
/// is empty, clamped into the texture.
[[nodiscard]] constexpr lx::rect2f effective_source(lx::rect2f src, unsigned tex_w,
                                                    unsigned tex_h) {
    const float tw = static_cast<float>(tex_w);
    const float th = static_cast<float>(tex_h);
    if (src.empty())
        return {0.f, 0.f, tw, th};
    float x0 = src.x < 0.f ? 0.f : src.x;
    float y0 = src.y < 0.f ? 0.f : src.y;
    float x1 = src.x + src.width > tw ? tw : src.x + src.width;
    float y1 = src.y + src.height > th ? th : src.y + src.height;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    return {x0, y0, x1 - x0, y1 - y0};
}

/// Downsampling (HiDPI supersampling — a client rendering at a render scale above the output
/// scale — and integer-scale clients on a fractional output). Bilinear sampling reads the
/// 2×2 texels around one point, so a draw that shrinks its source by more than a texel per
/// pixel weighs each pixel's footprint by where the pixel happens to fall: at 8:5 a
/// one-texel stroke comes out black or 60 % grey depending on its phase, and fine content
/// (text) shimmers as it moves. Such a draw uses the area (box) filter instead: each pixel is
/// the average of the source texels under its footprint, weighted by how much of each it
/// covers (cairo's GOOD filter shrinks with a box filter too), and the exact 2×2 average at
/// 2:1. On the GPUs each pair of neighbouring texels along an axis is one bilinear tap placed
/// at their coverage ratio, so a footprint of up to `k_max_downsample` texels per axis takes
/// at most 3×3 taps and a 2:1 footprint one (bilinear sampling itself). The CPU path weighs
/// the texels directly. Other draws sample exactly as before.
inline constexpr float k_max_downsample = 4.f;

/// A draw's footprint per destination pixel, in source texels along the source's own axes
/// (u: the buffer's x, v: its y).
struct downsample_footprint {
    float u = 1.f;
    float v = 1.f;
    /// More than a texel per pixel along either axis (1/64 texel of slack, so a fractional
    /// output's rounded logical size does not turn 1:1 into a filtered draw).
    [[nodiscard]] constexpr bool active() const {
        return u > 1.f + 1.f / 64.f || v > 1.f + 1.f / 64.f;
    }
};

/// The footprint of drawing source box `box` (texture pixels) into `dst` through buffer
/// transform `t`, each axis at most `k_max_downsample`. A 90° transform crosses the axes:
/// the box's width then spans `dst.height`. An axis shrunk exactly 2:1 from a whole texel
/// counts as 1: every pixel centre lands on a texel corner there, where the one bilinear
/// tap already is the 2×2 average — so a render scale of 2 on a scale-1 output (the common
/// case) is drawn as it always was, at no extra cost.
[[nodiscard]] constexpr downsample_footprint downsample_footprint_for(lx::rect2f box,
                                                                      lx::rect2i dst,
                                                                      lx::buffer_transform t) {
    if (dst.width <= 0 || dst.height <= 0)
        return {};
    const bool quarter = (static_cast<unsigned>(t) & 1u) != 0;
    const auto axis = [](float size, int span, float origin) {
        const float f = size / static_cast<float>(span);
        if (f == 2.f && origin == static_cast<float>(static_cast<long long>(origin)))
            return 1.f;
        return f < k_max_downsample ? f : k_max_downsample;
    };
    return {axis(box.width, quarter ? dst.height : dst.width, box.x),
            axis(box.height, quarter ? dst.width : dst.height, box.y)};
}

class pipeline_cache {
public:
    [[nodiscard]] unsigned acquire(unsigned blend_key, unsigned format_key);
    void clear();

private:
    unsigned next_id_ = 1;
    static constexpr unsigned k_capacity = 64;
    unsigned keys_[64]{};
    unsigned ids_[64]{};
    unsigned count_ = 0;
};

class frame_renderer {
public:
    void set_pipeline_cache(pipeline_cache* cache);

    [[nodiscard]] lx::result<void> draw_to_headless(const blit_command* cmds, unsigned count,
                                                     headless_backend& target);

private:
    pipeline_cache* pipelines_ = nullptr;
};

} // namespace lx::gfx


unsigned lx::gfx::pipeline_cache::acquire(unsigned blend_key, unsigned format_key) {
    const unsigned key = (blend_key << 16u) | (format_key & 0xFFFFu);
    for (unsigned i = 0; i < count_; ++i) {
        if (keys_[i] == key)
            return ids_[i];
    }
    if (count_ < k_capacity) {
        keys_[count_] = key;
        ids_[count_] = next_id_++;
        return ids_[count_++];
    }
    return next_id_++;
}

void lx::gfx::pipeline_cache::clear() { count_ = 0; }

bool lx::gfx::blur_planner::plan(std::span<const blur_source> sources, damage_region& damage,
                                 int width, int height) {
    const unsigned n = static_cast<unsigned>(
        sources.size() < k_max_blur_sources ? sources.size() : k_max_blur_sources);
    bool any = false;
    for (unsigned k = 0; k < n; ++k) {
        entry& e = entries_[k];
        const lx::rect2i box = sources[k].box;
        const lx::rect2i f = blur_footprint(box, width, height);
        const bool moved = k >= count_ || e.box.x != box.x || e.box.y != box.y ||
                           e.box.width != box.width || e.box.height != box.height ||
                           e.footprint.x != f.x || e.footprint.y != f.y ||
                           e.footprint.width != f.width || e.footprint.height != f.height;
        if (moved || e.backdrop != sources[k].backdrop)
            e.cached = false;
        e.box = box;
        e.footprint = f;
        e.backdrop = sources[k].backdrop;
        e.pending = !e.cached && !detail::damage_rect_empty(f);
        e.again = e.pending && sources[k].visible;
        any = any || e.again;
    }
    count_ = n;
    if (any)
        isolate(damage);
    return any;
}

void lx::gfx::blur_planner::isolate(damage_region& region) const {
    using namespace lx::gfx::detail;
    if (region.full)
        return;
    for (unsigned k = 0; k < count_; ++k) {
        if (!entries_[k].again)
            continue;
        // Now in some rect (merged or appended), so the pass below removes at least one and
        // the grown rect always fits back.
        region.add(entries_[k].footprint);
        lx::rect2i grown = entries_[k].footprint;
        for (bool met = true; met;) {
            met = false;
            unsigned kept = 0;
            for (unsigned i = 0; i < region.count; ++i) {
                if (!damage_rect_empty(damage_intersect(region.rects[i], grown))) {
                    grown = damage_bbox(grown, region.rects[i]);
                    met = true;
                } else {
                    region.rects[kept++] = region.rects[i];
                }
            }
            region.count = kept;
        }
        region.rects[region.count++] = grown;
    }
}

void lx::gfx::blur_planner::reset() {
    for (entry& e : entries_)
        e = {};
    count_ = 0;
}

void lx::gfx::frame_renderer::set_pipeline_cache(pipeline_cache* cache) { pipelines_ = cache; }

lx::result<void> lx::gfx::frame_renderer::draw_to_headless(const blit_command* cmds,
                                                              unsigned count,
                                                              headless_backend& target) {
    if (!cmds)
        return {};
    // Draw everything that resolves, but still report that something did not: an
    // unregistered texture means the commit and render paths disagree.
    lx::result<void> outcome{};
    for (unsigned i = 0; i < count; ++i) {
        const auto& cmd = cmds[i];
        if (pipelines_) {
            (void)pipelines_->acquire(static_cast<unsigned>(cmd.blend), 0);
        }
        if (cmd.occluded || cmd.blur || draw_invisible(cmd))
            continue;
        if (cmd.solid) {
            const float k = cmd.opacity * cmd.tint.a;
            const lx::color c = lx::color::rgb(cmd.solid_color.r * cmd.tint.r * k,
                                               cmd.solid_color.g * cmd.tint.g * k,
                                               cmd.solid_color.b * cmd.tint.b * k,
                                               cmd.solid_color.a * k);
            if (auto filled = target.fill_solid(cmd.dst, c); !filled)
                outcome = filled.get_error();
            continue;
        }
        if (auto blitted = target.blit_texture(cmd.texture_id, cmd.dst); !blitted)
            outcome = blitted.get_error();
    }
    return outcome;
}
