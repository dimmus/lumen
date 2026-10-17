module;

// wl_surface geometry with buffer scale, buffer transform and wp_viewport: the logical
// surface size, the buffer source box a draw samples, the protocol checks on them
// (wl_surface.invalid_size, wp_viewport.bad_size / out_of_buffer) and damage conversion
// between surface-local and buffer coordinates. Pure functions on plain values, no
// allocation, no Wayland types — the wire units feed them and tests call them directly.
//
// Semantics follow wlroots (`surface_finalize_pending`, `wlr_surface_get_buffer_source_box`,
// `surface_update_damage`, `wlr_surface_get_effective_damage`), which is what every client
// in the wild is tested against.

#include <cmath>
#include <cstdint>

export module lx.compositor:viewport;

import lx.foundation;

export namespace lx::compositor {

/// One surface state's geometry inputs. The viewport source is raw wl_fixed (24.8).
struct surface_geometry_in {
    /// The buffer that state shows, in pixels; 0x0 without one.
    int buffer_w = 0;
    int buffer_h = 0;
    int scale = 1;
    lx::buffer_transform transform = lx::buffer_transform::normal;
    bool has_src = false;
    std::int32_t src_x = 0;
    std::int32_t src_y = 0;
    std::int32_t src_w = 0;
    std::int32_t src_h = 0;
    bool has_dst = false;
    int dst_w = 0;
    int dst_h = 0;
};

struct surface_geometry {
    /// Logical surface size; 0x0 without a buffer.
    lx::size2i size{};
    /// The box of the buffer shown, in buffer pixels before the transform (empty: the
    /// whole buffer). Fractional edges come from a fractional viewport source.
    lx::rect2f src{};
    lx::buffer_transform transform = lx::buffer_transform::normal;
};

enum class viewport_error : unsigned char { none, bad_size, out_of_buffer };
enum class surface_size_error : unsigned char { none, invalid_size };

/// `w`×`h` turned by `t` (90 and 270 and their flips swap the sides).
[[nodiscard]] constexpr lx::size2i transformed_size(int w, int h, lx::buffer_transform t) {
    return (static_cast<unsigned>(t) & 1u) != 0 ? lx::size2i{h, w} : lx::size2i{w, h};
}

/// The transform that undoes `t` (90 ↔ 270; the others are their own inverse).
[[nodiscard]] constexpr lx::buffer_transform invert(lx::buffer_transform t) {
    if (t == lx::buffer_transform::rotate_90)
        return lx::buffer_transform::rotate_270;
    if (t == lx::buffer_transform::rotate_270)
        return lx::buffer_transform::rotate_90;
    return t;
}

/// wlroots' `wlr_box_transform`: where `r`, a box in a `w`×`h` space, lands once that space
/// is turned by `t`.
[[nodiscard]] lx::rect2f transform_box(lx::rect2f r, lx::buffer_transform t, float w, float h);
[[nodiscard]] lx::rect2i transform_box(lx::rect2i r, lx::buffer_transform t, int w, int h);

/// The viewport checks a commit makes on the state it produces (wp_viewport: "when
/// applied"; checked at the client's commit, as wlroots does):
///   bad_size — no destination, a source whose width or height is not an integer;
///   out_of_buffer — a source reaching past the buffer (in surface units: the buffer
///                   divided by the scale, transformed). Skipped without a buffer.
[[nodiscard]] viewport_error check_viewport(const surface_geometry_in& in);
/// wl_surface.invalid_size: without a viewport source, the buffer's sides must be multiples
/// of the scale.
[[nodiscard]] surface_size_error check_buffer_scale(const surface_geometry_in& in);
[[nodiscard]] surface_geometry compute_geometry(const surface_geometry_in& in);

/// Surface-local damage in buffer pixels (rounded out; clipped to the buffer): undo the
/// destination scale, shift by the source origin (floored — lossy, as in wlroots), scale
/// by the buffer scale, transform into the buffer.
[[nodiscard]] lx::rect2i surface_to_buffer_damage(lx::rect2i damage,
                                                  const surface_geometry_in& in);
/// Buffer damage in surface-local coordinates (rounded out; clipped to the surface): the
/// way back, cropping to the source.
[[nodiscard]] lx::rect2i buffer_to_surface_damage(lx::rect2i damage,
                                                  const surface_geometry_in& in);

/// Smallest rect covering both (an empty one is ignored), formed in 64 bits.
[[nodiscard]] lx::rect2i union_rect(lx::rect2i a, lx::rect2i b);
/// `r` ∩ `{0, 0, w, h}`; empty when they do not meet. INT32_MAX extents are fine.
[[nodiscard]] lx::rect2i clip_to(lx::rect2i r, int w, int h);

} // namespace lx::compositor

namespace {

[[nodiscard]] double fixed_to_double(std::int32_t f) { return static_cast<double>(f) / 256.0; }

[[nodiscard]] bool has_buffer(const lx::compositor::surface_geometry_in& in) {
    return in.buffer_w > 0 && in.buffer_h > 0;
}

[[nodiscard]] int scale_of(const lx::compositor::surface_geometry_in& in) {
    return in.scale > 0 ? in.scale : 1;
}

/// The viewport source size in surface units: the source, or the whole transformed buffer
/// divided by the scale.
void src_size(const lx::compositor::surface_geometry_in& in, double& w, double& h) {
    if (in.has_src) {
        w = fixed_to_double(in.src_w);
        h = fixed_to_double(in.src_h);
        return;
    }
    const lx::size2i t = lx::compositor::transformed_size(in.buffer_w, in.buffer_h, in.transform);
    w = static_cast<double>(t.width) / scale_of(in);
    h = static_cast<double>(t.height) / scale_of(in);
}

/// `{x0, y0, x1, y1}` in doubles, rounded out to whole pixels.
[[nodiscard]] lx::rect2i round_out(double x0, double y0, double x1, double y1) {
    constexpr double lo = -1.0e9, hi = 1.0e9;
    const auto clamp = [](double v) { return v < lo ? lo : (v > hi ? hi : v); };
    const long ix0 = static_cast<long>(std::floor(clamp(x0)));
    const long iy0 = static_cast<long>(std::floor(clamp(y0)));
    const long ix1 = static_cast<long>(std::ceil(clamp(x1)));
    const long iy1 = static_cast<long>(std::ceil(clamp(y1)));
    if (ix1 <= ix0 || iy1 <= iy0)
        return {};
    return {static_cast<int>(ix0), static_cast<int>(iy0), static_cast<int>(ix1 - ix0),
            static_cast<int>(iy1 - iy0)};
}

} // namespace

lx::rect2f lx::compositor::transform_box(lx::rect2f r, lx::buffer_transform t, float w,
                                         float h) {
    lx::rect2f d{};
    if ((static_cast<unsigned>(t) & 1u) == 0) {
        d.width = r.width;
        d.height = r.height;
    } else {
        d.width = r.height;
        d.height = r.width;
    }
    switch (t) {
    case lx::buffer_transform::normal:
        d.x = r.x;
        d.y = r.y;
        break;
    case lx::buffer_transform::rotate_90:
        d.x = h - r.y - r.height;
        d.y = r.x;
        break;
    case lx::buffer_transform::rotate_180:
        d.x = w - r.x - r.width;
        d.y = h - r.y - r.height;
        break;
    case lx::buffer_transform::rotate_270:
        d.x = r.y;
        d.y = w - r.x - r.width;
        break;
    case lx::buffer_transform::flipped:
        d.x = w - r.x - r.width;
        d.y = r.y;
        break;
    case lx::buffer_transform::flipped_90:
        d.x = r.y;
        d.y = r.x;
        break;
    case lx::buffer_transform::flipped_180:
        d.x = r.x;
        d.y = h - r.y - r.height;
        break;
    case lx::buffer_transform::flipped_270:
        d.x = h - r.y - r.height;
        d.y = w - r.x - r.width;
        break;
    }
    return d;
}

lx::rect2i lx::compositor::transform_box(lx::rect2i r, lx::buffer_transform t, int w, int h) {
    const lx::rect2f f = transform_box(lx::rect2f{r}, t, static_cast<float>(w),
                                       static_cast<float>(h));
    return {static_cast<int>(f.x), static_cast<int>(f.y), static_cast<int>(f.width),
            static_cast<int>(f.height)};
}

lx::compositor::viewport_error lx::compositor::check_viewport(const surface_geometry_in& in) {
    if (!in.has_src)
        return viewport_error::none;
    // wp_viewport.bad_size: "if no destination size is set and the source rectangle width
    // or height is not an integer". Raw 24.8: an integer has no fraction bits.
    if (!in.has_dst && ((in.src_w & 0xFF) != 0 || (in.src_h & 0xFF) != 0))
        return viewport_error::bad_size;
    if (!has_buffer(in))
        return viewport_error::none;
    // out_of_buffer: x + width > W/scale (or y + height), compared exactly in integers:
    // (x + w)·scale > W·256.
    const lx::size2i t = transformed_size(in.buffer_w, in.buffer_h, in.transform);
    const long long sc = scale_of(in);
    const long long right = (static_cast<long long>(in.src_x) + in.src_w) * sc;
    const long long bottom = (static_cast<long long>(in.src_y) + in.src_h) * sc;
    if (right > static_cast<long long>(t.width) * 256 ||
        bottom > static_cast<long long>(t.height) * 256)
        return viewport_error::out_of_buffer;
    return viewport_error::none;
}

lx::compositor::surface_size_error
lx::compositor::check_buffer_scale(const surface_geometry_in& in) {
    if (in.has_src || !has_buffer(in))
        return surface_size_error::none;
    const int s = scale_of(in);
    if (in.buffer_w % s != 0 || in.buffer_h % s != 0)
        return surface_size_error::invalid_size;
    return surface_size_error::none;
}

lx::compositor::surface_geometry
lx::compositor::compute_geometry(const surface_geometry_in& in) {
    surface_geometry g{};
    g.transform = in.transform;
    if (!has_buffer(in))
        return g;
    const int s = scale_of(in);
    const lx::size2i t = transformed_size(in.buffer_w, in.buffer_h, in.transform);
    if (in.has_dst) {
        g.size = {in.dst_w, in.dst_h};
    } else if (in.has_src) {
        // bad_size guarantees integers here; truncation never drops a fraction.
        g.size = {static_cast<int>(in.src_w >> 8), static_cast<int>(in.src_h >> 8)};
    } else {
        // invalid_size guarantees exact division for a role that is drawn; a tolerated
        // cursor rounds down, as wlroots does.
        g.size = {t.width / s, t.height / s};
    }
    if (in.has_src) {
        // The source in transformed-buffer pixels, then into the buffer itself.
        const lx::rect2f box{static_cast<float>(fixed_to_double(in.src_x) * s),
                             static_cast<float>(fixed_to_double(in.src_y) * s),
                             static_cast<float>(fixed_to_double(in.src_w) * s),
                             static_cast<float>(fixed_to_double(in.src_h) * s)};
        g.src = transform_box(box, invert(in.transform), static_cast<float>(t.width),
                              static_cast<float>(t.height));
    }
    if (g.size.width <= 0 || g.size.height <= 0)
        g.size = {};
    return g;
}

lx::rect2i lx::compositor::union_rect(lx::rect2i a, lx::rect2i b) {
    if (a.width <= 0 || a.height <= 0)
        return b;
    if (b.width <= 0 || b.height <= 0)
        return a;
    const long long x0 = a.x < b.x ? a.x : b.x;
    const long long y0 = a.y < b.y ? a.y : b.y;
    const long long ax1 = static_cast<long long>(a.x) + a.width;
    const long long bx1 = static_cast<long long>(b.x) + b.width;
    const long long ay1 = static_cast<long long>(a.y) + a.height;
    const long long by1 = static_cast<long long>(b.y) + b.height;
    const long long x1 = ax1 > bx1 ? ax1 : bx1;
    const long long y1 = ay1 > by1 ? ay1 : by1;
    const auto cap = [](long long v) { return v > 0x7FFFFFFFLL ? 0x7FFFFFFFLL : v; };
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(cap(x1 - x0)),
            static_cast<int>(cap(y1 - y0))};
}

lx::rect2i lx::compositor::clip_to(lx::rect2i r, int w, int h) {
    const long long x0 = r.x > 0 ? r.x : 0;
    const long long y0 = r.y > 0 ? r.y : 0;
    long long x1 = static_cast<long long>(r.x) + r.width;
    long long y1 = static_cast<long long>(r.y) + r.height;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

lx::rect2i lx::compositor::surface_to_buffer_damage(lx::rect2i damage,
                                                    const surface_geometry_in& in) {
    if (!has_buffer(in) || damage.width <= 0 || damage.height <= 0)
        return {};
    double x0 = damage.x, y0 = damage.y;
    double x1 = static_cast<double>(damage.x) + damage.width;
    double y1 = static_cast<double>(damage.y) + damage.height;
    if (in.has_dst && in.dst_w > 0 && in.dst_h > 0) {
        double sw = 0.0, sh = 0.0;
        src_size(in, sw, sh);
        const double kx = sw / in.dst_w, ky = sh / in.dst_h;
        x0 *= kx, x1 *= kx, y0 *= ky, y1 *= ky;
    }
    if (in.has_src) {
        const double ox = std::floor(fixed_to_double(in.src_x));
        const double oy = std::floor(fixed_to_double(in.src_y));
        x0 += ox, x1 += ox, y0 += oy, y1 += oy;
    }
    const int s = scale_of(in);
    lx::rect2i r = round_out(x0 * s, y0 * s, x1 * s, y1 * s);
    const lx::size2i t = transformed_size(in.buffer_w, in.buffer_h, in.transform);
    r = clip_to(r, t.width, t.height);
    if (r.width <= 0)
        return {};
    r = transform_box(r, invert(in.transform), t.width, t.height);
    return clip_to(r, in.buffer_w, in.buffer_h);
}

lx::rect2i lx::compositor::buffer_to_surface_damage(lx::rect2i damage,
                                                    const surface_geometry_in& in) {
    if (!has_buffer(in))
        return {};
    lx::rect2i r = clip_to(damage, in.buffer_w, in.buffer_h);
    if (r.width <= 0)
        return {};
    r = transform_box(r, in.transform, in.buffer_w, in.buffer_h);
    const int s = scale_of(in);
    double x0 = static_cast<double>(r.x) / s, y0 = static_cast<double>(r.y) / s;
    double x1 = static_cast<double>(r.x + r.width) / s;
    double y1 = static_cast<double>(r.y + r.height) / s;
    double sw = 0.0, sh = 0.0;
    src_size(in, sw, sh);
    if (in.has_src) {
        // Cropped to the source, whose origin becomes the surface's.
        const double ox = std::floor(fixed_to_double(in.src_x));
        const double oy = std::floor(fixed_to_double(in.src_y));
        x0 -= ox, x1 -= ox, y0 -= oy, y1 -= oy;
    }
    if (in.has_dst && in.dst_w > 0 && in.dst_h > 0 && sw > 0.0 && sh > 0.0) {
        const double kx = in.dst_w / sw, ky = in.dst_h / sh;
        x0 *= kx, x1 *= kx, y0 *= ky, y1 *= ky;
    }
    const surface_geometry g = compute_geometry(in);
    return clip_to(round_out(x0, y0, x1, y1), g.size.width, g.size.height);
}
