module;

// Bodies of `:compose`. See compose.cppm for the model.

module lx.portal;

import std;

import lx.foundation;
import :compose;

namespace lx::portal {

namespace {

/// lx.gfx `source_basis_for`: where a content point lands in the output's pixels, as
/// origin + x·dx + y·dy in units of the output's own width/height. Integer here (every
/// entry is 0 or ±1); test_portal_compose checks it against lx.gfx's `transform_rect`.
struct basis {
    int ox, oy;   // origin, in output widths/heights
    int dxx, dxy; // +x direction
    int dyx, dyy; // +y direction
};

constexpr basis basis_for(lx::buffer_transform t) {
    switch (t) {
    case lx::buffer_transform::normal: return {0, 0, 1, 0, 0, 1};
    case lx::buffer_transform::rotate_90: return {0, 1, 0, -1, 1, 0};
    case lx::buffer_transform::rotate_180: return {1, 1, -1, 0, 0, -1};
    case lx::buffer_transform::rotate_270: return {1, 0, 0, 1, -1, 0};
    case lx::buffer_transform::flipped: return {1, 0, -1, 0, 0, 1};
    case lx::buffer_transform::flipped_90: return {0, 0, 0, 1, 1, 0};
    case lx::buffer_transform::flipped_180: return {0, 1, 1, 0, 0, -1};
    case lx::buffer_transform::flipped_270: return {1, 1, 0, -1, -1, 0};
    }
    return {0, 0, 1, 0, 0, 1};
}

constexpr bool turned(lx::buffer_transform t) { return (static_cast<unsigned>(t) & 1u) != 0; }

lx::rect2i intersect(lx::rect2i a, lx::rect2i b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.width, b.x + b.width);
    const int y1 = std::min(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

bool whole(double v) { return std::fabs(v - std::round(v)) < 1e-6; }

std::uint32_t lerp_px(std::uint32_t a, std::uint32_t b, unsigned w /*0..256*/) {
    // Two channels at a time (R|B and G), 8-bit weights: exact for w = 0 and 256.
    const std::uint32_t rb = ((a & 0xFF00FFu) * (256u - w) + (b & 0xFF00FFu) * w) >> 8;
    const std::uint32_t g = ((a & 0x00FF00u) * (256u - w) + (b & 0x00FF00u) * w) >> 8;
    return (rb & 0xFF00FFu) | (g & 0x00FF00u);
}

} // namespace

lx::size2i upright_size(lx::size2i buffer, lx::buffer_transform transform) {
    return turned(transform) ? lx::size2i{buffer.height, buffer.width} : buffer;
}

lx::point2i buffer_pixel_of(int x, int y, lx::buffer_transform transform, int width, int height) {
    const basis b = basis_for(transform);
    const long long fw = turned(transform) ? height : width;
    const long long fh = turned(transform) ? width : height;
    // The pixel's two corners mapped; the pixel is the box between them.
    const long long ax = b.ox * fw + static_cast<long long>(b.dxx) * x + static_cast<long long>(b.dyx) * y;
    const long long ay = b.oy * fh + static_cast<long long>(b.dxy) * x + static_cast<long long>(b.dyy) * y;
    const long long bx = ax + b.dxx + b.dyx;
    const long long by = ay + b.dxy + b.dyy;
    return {static_cast<int>(std::min(ax, bx)), static_cast<int>(std::min(ay, by))};
}

image upright(const std::uint32_t* buffer, int width, int height, int stride_px,
              lx::buffer_transform transform) {
    image out;
    if (!buffer || width <= 0 || height <= 0 || stride_px < width)
        return out;
    const lx::size2i up = upright_size({width, height}, transform);
    out.allocate(up.width, up.height);
    if (transform == lx::buffer_transform::normal) {
        for (int y = 0; y < height; ++y) {
            const std::uint32_t* src = buffer + static_cast<std::size_t>(y) * stride_px;
            std::uint32_t* dst = out.pixels.data() + static_cast<std::size_t>(y) * width;
            for (int x = 0; x < width; ++x)
                dst[x] = src[x] | 0xFF000000u;
        }
        return out;
    }
    // Per layout pixel: the buffer pixel the compositor put it in. The map is affine, so
    // walk it: the buffer step per layout x and per layout y are constant.
    const basis b = basis_for(transform);
    const lx::point2i p00 = buffer_pixel_of(0, 0, transform, up.width, up.height);
    const long long step_x = static_cast<long long>(b.dxy) * stride_px + b.dxx;
    const long long step_y = static_cast<long long>(b.dyy) * stride_px + b.dyx;
    long long row = static_cast<long long>(p00.y) * stride_px + p00.x;
    for (int y = 0; y < up.height; ++y, row += step_y) {
        std::uint32_t* dst = out.pixels.data() + static_cast<std::size_t>(y) * up.width;
        long long at = row;
        for (int x = 0; x < up.width; ++x, at += step_x)
            dst[x] = buffer[at] | 0xFF000000u;
    }
    return out;
}

double shot_scale(const output_shot& shot) {
    if (shot.logical.width <= 0 || shot.pixels.width <= 0)
        return 1.0;
    return static_cast<double>(shot.pixels.width) / shot.logical.width;
}

lx::rect2i layout_bounds(std::span<const output_shot> shots) {
    bool any = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const output_shot& s : shots) {
        if (s.logical.width <= 0 || s.logical.height <= 0)
            continue;
        if (!any) {
            x0 = s.logical.x;
            y0 = s.logical.y;
            x1 = s.logical.x + s.logical.width;
            y1 = s.logical.y + s.logical.height;
            any = true;
            continue;
        }
        x0 = std::min(x0, s.logical.x);
        y0 = std::min(y0, s.logical.y);
        x1 = std::max(x1, s.logical.x + s.logical.width);
        y1 = std::max(y1, s.logical.y + s.logical.height);
    }
    return any ? lx::rect2i{x0, y0, x1 - x0, y1 - y0} : lx::rect2i{};
}

double compose_scale(std::span<const output_shot> shots, lx::rect2i region) {
    double best = 0.0;
    for (const output_shot& s : shots)
        if (intersect(s.logical, region).width > 0)
            best = std::max(best, shot_scale(s));
    return best > 0.0 ? best : 1.0;
}

lx::size2i compose_size(lx::rect2i region, double scale) {
    return {static_cast<int>(std::lround(region.width * scale)),
            static_cast<int>(std::lround(region.height * scale))};
}

image compose(std::span<const output_shot> shots, lx::rect2i region, double scale) {
    image out;
    if (region.width <= 0 || region.height <= 0 || !(scale > 0.0))
        return out;
    const lx::size2i size = compose_size(region, scale);
    out.allocate(size.width, size.height);
    if (out.empty())
        return out;
    std::vector<int> xi;
    std::vector<unsigned> xw;
    for (const output_shot& s : shots) {
        const lx::rect2i in = intersect(s.logical, region);
        if (in.width <= 0 || s.pixels.empty())
            continue;
        // Destination pixels of the intersection, edges rounded the same way for every
        // shot so neighbours meet without a gap or an overlap.
        const int dx0 = static_cast<int>(std::lround((in.x - region.x) * scale));
        const int dy0 = static_cast<int>(std::lround((in.y - region.y) * scale));
        const int dx1 = std::min(size.width, static_cast<int>(std::lround((in.x + in.width - region.x) * scale)));
        const int dy1 = std::min(size.height, static_cast<int>(std::lround((in.y + in.height - region.y) * scale)));
        if (dx1 <= dx0 || dy1 <= dy0)
            continue;
        const double sx_scale = static_cast<double>(s.pixels.width) / s.logical.width;
        const double sy_scale = static_cast<double>(s.pixels.height) / s.logical.height;
        const double ox = (s.logical.x - region.x) * scale; // the shot's origin, dest pixels
        const double oy = (s.logical.y - region.y) * scale;
        if (std::fabs(sx_scale - scale) < 1e-9 && std::fabs(sy_scale - scale) < 1e-9 && whole(ox) &&
            whole(oy)) {
            // 1:1 — the shot's pixels themselves.
            const int ix = static_cast<int>(std::lround(ox));
            const int iy = static_cast<int>(std::lround(oy));
            for (int y = dy0; y < dy1; ++y) {
                const int sy = y - iy;
                if (sy < 0 || sy >= s.pixels.height)
                    continue;
                const int sx0 = std::max(0, dx0 - ix);
                const int sx1 = std::min(s.pixels.width, dx1 - ix);
                if (sx1 <= sx0)
                    continue;
                std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * size.width + (sx0 + ix),
                            s.pixels.pixels.data() + static_cast<std::size_t>(sy) * s.pixels.width + sx0,
                            static_cast<std::size_t>(sx1 - sx0) * 4u);
            }
            continue;
        }
        // Bilinear: a destination pixel centre → layout point → source pixel space.
        const auto src_coord = [&](int d, double org, double sscale, int limit, int& i0,
                                   unsigned& w) {
            const double logical_off = (d + 0.5 - org) / scale; // from the shot's origin
            double sc = logical_off * sscale - 0.5;
            if (sc < 0.0)
                sc = 0.0;
            const double maxc = limit - 1;
            if (sc > maxc)
                sc = maxc;
            i0 = static_cast<int>(sc);
            w = static_cast<unsigned>(std::lround((sc - i0) * 256.0));
            if (i0 >= limit - 1) {
                i0 = limit - 1;
                w = 0;
            }
        };
        xi.resize(static_cast<std::size_t>(dx1 - dx0));
        xw.resize(xi.size());
        for (int x = dx0; x < dx1; ++x)
            src_coord(x, ox, sx_scale, s.pixels.width, xi[x - dx0], xw[x - dx0]);
        for (int y = dy0; y < dy1; ++y) {
            int sy = 0;
            unsigned wy = 0;
            src_coord(y, oy, sy_scale, s.pixels.height, sy, wy);
            const std::uint32_t* r0 = s.pixels.pixels.data() + static_cast<std::size_t>(sy) * s.pixels.width;
            const std::uint32_t* r1 = sy + 1 < s.pixels.height ? r0 + s.pixels.width : r0;
            std::uint32_t* dst = out.pixels.data() + static_cast<std::size_t>(y) * size.width;
            for (int x = dx0; x < dx1; ++x) {
                const int sx = xi[x - dx0];
                const unsigned wx = xw[x - dx0];
                const int sx1 = sx + 1 < s.pixels.width ? sx + 1 : sx;
                const std::uint32_t top = lerp_px(r0[sx], r0[sx1], wx);
                const std::uint32_t bottom = lerp_px(r1[sx], r1[sx1], wx);
                dst[x] = lerp_px(top, bottom, wy) | 0xFF000000u;
            }
        }
    }
    return out;
}

int shot_at(std::span<const output_shot> shots, double x, double y) {
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const lx::rect2i& r = shots[i].logical;
        if (x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height)
            return static_cast<int>(i);
    }
    return -1;
}

bool colour_at(std::span<const output_shot> shots, double x, double y, std::uint32_t& argb) {
    const int i = shot_at(shots, x, y);
    if (i < 0)
        return false;
    const output_shot& s = shots[static_cast<std::size_t>(i)];
    if (s.pixels.empty())
        return false;
    const double sx_scale = static_cast<double>(s.pixels.width) / s.logical.width;
    const double sy_scale = static_cast<double>(s.pixels.height) / s.logical.height;
    const int px = std::clamp(static_cast<int>(std::floor((x - s.logical.x) * sx_scale)), 0,
                              s.pixels.width - 1);
    const int py = std::clamp(static_cast<int>(std::floor((y - s.logical.y) * sy_scale)), 0,
                              s.pixels.height - 1);
    argb = s.pixels.at(px, py) | 0xFF000000u;
    return true;
}

void dim(image& img, unsigned num) {
    for (std::uint32_t& p : img.pixels) {
        const std::uint32_t rb = ((p & 0xFF00FFu) * num) >> 8;
        const std::uint32_t g = ((p & 0x00FF00u) * num) >> 8;
        p = 0xFF000000u | (rb & 0xFF00FFu) | (g & 0x00FF00u);
    }
}

} // namespace lx::portal
