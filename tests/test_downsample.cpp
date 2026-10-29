// Downsampling a client buffer onto an output (HiDPI supersampling: `render_scale` above
// the output scale), on each composite backend — CPU, GL (a hardware render node) and
// Vulkan (lavapipe will do).
//
// The reference is the exact area (box) filter: each destination pixel is the coverage-
// weighted average of the source texels under its footprint. Plain bilinear sampling is that
// filter only at exactly 2:1; at a non-integer ratio (2 → 1.5, 2 → 1.25) or 3:1 it reads
// 2×2 texels of a footprint that spans more, skips the rest, and a fine pattern (text)
// aliases and shimmers as it moves. Each case prints the RMS and largest error against the
// reference, and the error's spread over shifts of the pattern (shimmer). Measured on the
// dev VM with plain bilinear (before the filter), RMS / max of 255: 4:3 10.2 / 33,
// 8:5 24.0 / 102, 3:1 83.5 / 220, 4:1 55.2 / 184 — with it, under 0.5 / 1.2 everywhere.
//
//   - a render_scale = 2 client on a scale-1 output: a 2W×2H buffer drawn into W×H is the
//     2×2 average of its texels, on every backend (the blit_command the compositor makes
//     for it: the whole buffer into the output-pixel rect of its logical size);
//   - 1:1 and magnification are unchanged (the identity copy; bilinear upscale);
//   - non-integer, 3:1 and 4:1 ratios, and a 90°-turned buffer, follow the area filter.
#include "lumen_test.hpp"

#include <fcntl.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);

/// A grey test image: `v[y * w + x]` 0..255.
struct grey {
    unsigned w = 0, h = 0;
    std::vector<unsigned char> v;
    [[nodiscard]] unsigned char at(unsigned x, unsigned y) const { return v[y * w + x]; }
};

/// Text-like content: 1- and 2-texel strokes in both directions, a diagonal, and a ramp —
/// the frequencies that alias first. `shift` moves it by whole texels (sub-pixel at the
/// output after downsampling).
grey pattern(unsigned w, unsigned h, unsigned shift = 0) {
    grey g{w, h, std::vector<unsigned char>(static_cast<std::size_t>(w) * h)};
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            const unsigned sx = x + shift;
            const unsigned sy = y + shift;
            unsigned char c = 255;
            if (sx % 7 == 0 || (sx % 11) < 2)
                c = 0; // vertical strokes, 1 and 2 texels wide
            if (sy % 5 == 0 || (sy % 13) < 2)
                c = c ? 30 : 0; // horizontal strokes
            if ((sx + 2 * sy) % 17 == 0)
                c = 90; // a slanted stroke
            if (sy > h * 3 / 4)
                c = static_cast<unsigned char>((sx * 255u) / (w - 1)); // a smooth ramp
            g.v[static_cast<std::size_t>(y) * w + x] = c;
        }
    }
    return g;
}

/// The exact area filter of `src` (sw×sh) onto dw×dh, in the encoded values the samplers
/// average (an opaque sRGB draw decodes and re-encodes, the identity).
std::vector<double> area_reference(const grey& src, unsigned dw, unsigned dh) {
    std::vector<double> out(static_cast<std::size_t>(dw) * dh);
    const double fx = static_cast<double>(src.w) / dw;
    const double fy = static_cast<double>(src.h) / dh;
    for (unsigned y = 0; y < dh; ++y) {
        for (unsigned x = 0; x < dw; ++x) {
            const double x0 = x * fx, x1 = (x + 1) * fx, y0 = y * fy, y1 = (y + 1) * fy;
            double sum = 0.0, area = 0.0;
            for (unsigned ty = static_cast<unsigned>(y0); ty < src.h && ty < y1; ++ty) {
                const double wy = std::min<double>(ty + 1, y1) - std::max<double>(ty, y0);
                for (unsigned tx = static_cast<unsigned>(x0); tx < src.w && tx < x1; ++tx) {
                    const double wx = std::min<double>(tx + 1, x1) - std::max<double>(tx, x0);
                    sum += wx * wy * src.at(tx, ty);
                    area += wx * wy;
                }
            }
            out[static_cast<std::size_t>(y) * dw + x] = area > 0.0 ? sum / area : 0.0;
        }
    }
    return out;
}

struct error_stats {
    double rms = 0.0;
    double max = 0.0;
};

/// RMS and largest difference of `got` (grey, dw×dh) against `want`, away from a one-pixel
/// border (edge clamping is a separate question).
error_stats compare(const std::vector<unsigned char>& got, const std::vector<double>& want,
                    unsigned dw, unsigned dh) {
    error_stats e{};
    double sq = 0.0;
    unsigned n = 0;
    for (unsigned y = 1; y + 1 < dh; ++y) {
        for (unsigned x = 1; x + 1 < dw; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * dw + x;
            const double d = std::abs(static_cast<double>(got[i]) - want[i]);
            sq += d * d;
            e.max = std::max(e.max, d);
            ++n;
        }
    }
    e.rms = n ? std::sqrt(sq / n) : 0.0;
    return e;
}

/// A backend that composites one full-target draw of a grey image and reads it back grey.
struct backend {
    const char* name = "";
    /// Composites `src` stretched over a dw×dh target; false when the backend cannot.
    std::function<bool(const grey& src, unsigned dw, unsigned dh, lx::buffer_transform t,
                       std::vector<unsigned char>&)>
        draw;
    [[nodiscard]] bool operator()(const grey& src, unsigned dw, unsigned dh,
                                  std::vector<unsigned char>& out,
                                  lx::buffer_transform t = lx::buffer_transform::normal) const {
        return draw(src, dw, dh, t, out);
    }
};

lx::gfx::blit_command full_draw(unsigned texture_id, unsigned dw, unsigned dh,
                                lx::buffer_transform t) {
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.buffer_xform = t;
    cmd.dst = {0, 0, static_cast<int>(dw), static_cast<int>(dh)};
    cmd.blend = lx::blend_mode::opaque;
    cmd.opacity = 1.f;
    return cmd;
}

std::vector<unsigned char> to_rgba(const grey& g) {
    std::vector<unsigned char> rgba(g.v.size() * 4);
    for (std::size_t i = 0; i < g.v.size(); ++i) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g.v[i];
        rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

bool cpu_draw(const grey& src, unsigned dw, unsigned dh, lx::buffer_transform t,
              std::vector<unsigned char>& out) {
    std::vector<std::uint32_t> sp(src.v.size());
    for (std::size_t i = 0; i < sp.size(); ++i)
        sp[i] = 0xFF000000u | (static_cast<std::uint32_t>(src.v[i]) * 0x010101u);
    std::vector<std::uint32_t> dp(static_cast<std::size_t>(dw) * dh, 0xFFFF00FFu);
    lx::gfx::pixel_surface s{reinterpret_cast<unsigned char*>(sp.data()), src.w, src.h,
                             src.w * 4u, k_xrgb};
    lx::gfx::pixel_surface d{reinterpret_cast<unsigned char*>(dp.data()), dw, dh, dw * 4u,
                             k_xrgb};
    lx::gfx::cpu_compositor comp;
    if (!comp.register_texture(1, s))
        return false;
    const auto cmd = full_draw(1, dw, dh, t);
    if (!comp.composite(d, lx::color::rgb(1.f, 0.f, 1.f), &cmd, 1, {}))
        return false;
    out.resize(dp.size());
    for (std::size_t i = 0; i < dp.size(); ++i)
        out[i] = static_cast<unsigned char>((dp[i] >> 8) & 0xFFu); // G
    return true;
}

lx::gfx::egl_device* gl_device() {
    static lx::gfx::egl_device device{};
    static int state = 0; // 0 untried, 1 usable, 2 not
    if (state == 0) {
        state = 2;
        const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0)
            return nullptr;
        auto created = lx::gfx::egl_device::create(fd);
        if (!created || created.value().is_software_renderer())
            return nullptr;
        device = std::move(created).value();
        state = 1;
        std::printf("  GL renderer: %s\n", device.renderer());
    }
    return state == 1 ? &device : nullptr;
}

bool gl_draw(const grey& src, unsigned dw, unsigned dh, lx::buffer_transform t,
             std::vector<unsigned char>& out) {
    auto* device = gl_device();
    if (!device)
        return false;
    auto target = lx::gfx::gl_scanout_target::create(*device, dw, dh, k_xrgb);
    if (!target)
        return false;
    lx::gfx::gl_compositor comp;
    if (!comp.initialize(*device))
        return false;
    const auto rgba = to_rgba(src);
    if (!comp.upload_rgba(1, src.w, src.h, rgba.data()))
        return false;
    const auto cmd = full_draw(1, dw, dh, t);
    auto stats = comp.composite(target.value(), lx::color::rgb(1.f, 0.f, 1.f), &cmd, 1, {});
    if (!stats)
        return false;
    if (stats.value().out_fence_fd >= 0)
        ::close(stats.value().out_fence_fd);
    std::vector<unsigned char> px(static_cast<std::size_t>(dw) * dh * 4);
    if (!comp.read_back(target.value(), px.data(), static_cast<unsigned>(px.size())))
        return false;
    out.resize(static_cast<std::size_t>(dw) * dh);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = px[i * 4 + 1];
    return true;
}

struct vk_fixture {
    lx::gfx::device device{};
    lx::gfx::vulkan_compositor compositor{};
    int state = 0;
};

vk_fixture& vk() {
    static vk_fixture f{};
    if (f.state == 0) {
        f.state = 2;
        auto selected = lx::gfx::device_selector::select_best();
        if (!selected)
            return f;
        f.device = std::move(selected).value();
        if (!f.compositor.initialize(f.device.context()))
            return f;
        f.state = 1;
        std::printf("  Vulkan device: %s\n", f.device.info().device_name);
    }
    return f;
}

bool vk_draw(const grey& src, unsigned dw, unsigned dh, lx::buffer_transform t,
             std::vector<unsigned char>& out) {
    vk_fixture& f = vk();
    if (f.state != 1)
        return false;
    auto target = lx::gfx::render_target::create(f.device.context(), dw, dh);
    if (!target)
        return false;
    static unsigned next_id = 1;
    const unsigned id = next_id++;
    const auto rgba = to_rgba(src);
    if (!f.compositor.upload_rgba(id, src.w, src.h, rgba.data()))
        return false;
    const auto cmd = full_draw(id, dw, dh, t);
    auto stats = f.compositor.composite(target.value(), lx::color::rgb(1.f, 0.f, 1.f), &cmd, 1);
    if (!stats)
        return false;
    if (stats.value().out_fence_fd >= 0)
        ::close(stats.value().out_fence_fd);
    std::vector<unsigned char> px(static_cast<std::size_t>(dw) * dh * 4);
    const bool read =
        f.compositor.read_back(target.value(), px.data(), static_cast<unsigned>(px.size()))
            .has_value();
    f.compositor.forget_texture(id);
    if (!read)
        return false;
    out.resize(static_cast<std::size_t>(dw) * dh);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = px[i * 4 + 1];
    return true;
}

std::vector<backend> backends() {
    std::vector<backend> b;
    b.push_back({"cpu", cpu_draw});
    b.push_back({"gl", gl_draw});
    b.push_back({"vulkan", vk_draw});
    return b;
}

struct ratio_case {
    const char* what;
    unsigned sw, sh, dw, dh;
};

/// RMS against the area filter over `shifts` sub-texel positions of the pattern, and the
/// spread of the per-shift RMS (how much the error moves as content scrolls: shimmer).
struct case_result {
    error_stats worst{};
    double shimmer = 0.0;
    bool ran = false;
};

case_result run_case(const backend& b, const ratio_case& c, unsigned shifts) {
    case_result r{};
    double lo = 1e9, hi = 0.0;
    for (unsigned s = 0; s < shifts; ++s) {
        const grey src = pattern(c.sw, c.sh, s);
        std::vector<unsigned char> got;
        if (!b(src, c.dw, c.dh, got))
            return r;
        const error_stats e = compare(got, area_reference(src, c.dw, c.dh), c.dw, c.dh);
        r.worst.rms = std::max(r.worst.rms, e.rms);
        r.worst.max = std::max(r.worst.max, e.max);
        lo = std::min(lo, e.rms);
        hi = std::max(hi, e.rms);
    }
    r.shimmer = hi - lo;
    r.ran = true;
    return r;
}

} // namespace

// A render_scale = 2 client on a scale-1 output: every output pixel is the average of the
// 2×2 texels it covers.
LUMEN_TEST(render_scale_2_on_scale_1_is_the_2x2_average) {
    constexpr unsigned W = 48, H = 32;
    const grey src = pattern(2 * W, 2 * H);
    for (const backend& b : backends()) {
        std::vector<unsigned char> got;
        if (!b(src, W, H, got)) {
            std::printf("  (%s: skipped, backend unavailable)\n", b.name);
            continue;
        }
        int worst = 0;
        for (unsigned y = 0; y < H; ++y) {
            for (unsigned x = 0; x < W; ++x) {
                const int want = (src.at(2 * x, 2 * y) + src.at(2 * x + 1, 2 * y) +
                                  src.at(2 * x, 2 * y + 1) + src.at(2 * x + 1, 2 * y + 1) + 2) /
                                 4;
                worst = std::max(worst, std::abs(static_cast<int>(got[y * W + x]) - want));
            }
        }
        std::printf("  %-6s 2:1  largest difference from the 2x2 average %d\n", b.name, worst);
        // 8-bit sampler weights round; nothing else may differ.
        LUMEN_CHECK(worst <= 2);
    }
}

// 1:1 is an exact copy and magnification is untouched by the downsample filter.
LUMEN_TEST(identity_and_magnification_are_unchanged) {
    const grey src = pattern(40, 24);
    for (const backend& b : backends()) {
        std::vector<unsigned char> got;
        if (!b(src, 40, 24, got)) {
            std::printf("  (%s: skipped, backend unavailable)\n", b.name);
            continue;
        }
        int worst = 0;
        for (std::size_t i = 0; i < got.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<int>(got[i]) - src.v[i]));
        LUMEN_CHECK(worst <= 1);
        // 2× magnification of a 2-texel-period stripe, which the downsample filter must
        // leave alone: whole texels repeated on the CPU (its exact integer magnification),
        // bilinear on the GPUs — pixel 1 samples a quarter of the way from texel 0 to 1.
        grey stripes{4, 2, {0, 255, 0, 255, 0, 255, 0, 255}};
        std::vector<unsigned char> up;
        LUMEN_CHECK(b(stripes, 8, 4, up));
        const bool nearest = std::strcmp(b.name, "cpu") == 0;
        LUMEN_CHECK(std::abs(static_cast<int>(up[1]) - (nearest ? 0 : 64)) <= 3 &&
                    std::abs(static_cast<int>(up[2]) - (nearest ? 255 : 191)) <= 3);
    }
}

// The area filter at the ratios supersampling produces, and the shimmer (error spread as the
// content moves by a texel). Within 8-bit rounding of the reference: RMS under 1, no pixel
// more than 2 off (GPU samplers keep 8 bits of sub-texel position).
LUMEN_TEST(non_integer_downsampling_follows_the_area_filter) {
    const ratio_case cases[] = {
        {"2 -> 1.5 (4:3)", 192, 128, 144, 96},
        {"2 -> 1.25 (8:5)", 160, 120, 100, 75},
        {"2 -> 1 (2:1)", 192, 128, 96, 64},
        {"3 -> 1 (3:1)", 192, 132, 64, 44},
        {"4 -> 1 (4:1)", 192, 128, 48, 32},
    };
    for (const backend& b : backends()) {
        for (const ratio_case& c : cases) {
            const case_result r = run_case(b, c, 4);
            if (!r.ran) {
                std::printf("  (%s: skipped, backend unavailable)\n", b.name);
                break;
            }
            std::printf("  %-6s %-16s rms %5.2f  max %5.1f  shimmer %5.2f\n", b.name, c.what,
                        r.worst.rms, r.worst.max, r.shimmer);
            LUMEN_CHECK(r.worst.rms <= 1.0 && r.worst.max <= 2.0);
        }
    }
}

// A buffer turned 90° (wl_surface.set_buffer_transform): its width spans the output's
// height, so the footprint's axes cross. Drawn turned back, it must match the area filter of
// the unturned content.
LUMEN_TEST(turned_buffer_downsamples_along_its_own_axes) {
    // Content 192×128 shown at 144×96 (4:3); the buffer holds it turned: 128×192, where
    // rotate_90 shows content pixel (cx, cy) from buffer texel (cy, W − 1 − cx).
    const grey content = pattern(192, 128);
    grey buffer{content.h, content.w, std::vector<unsigned char>(content.v.size())};
    for (unsigned yb = 0; yb < buffer.h; ++yb)
        for (unsigned xb = 0; xb < buffer.w; ++xb)
            buffer.v[static_cast<std::size_t>(yb) * buffer.w + xb] =
                content.at(content.w - 1 - yb, xb);
    const std::vector<double> want = area_reference(content, 144, 96);
    for (const backend& b : backends()) {
        std::vector<unsigned char> got;
        if (!b(buffer, 144, 96, got, lx::buffer_transform::rotate_90)) {
            std::printf("  (%s: skipped, backend unavailable)\n", b.name);
            continue;
        }
        const error_stats e = compare(got, want, 144, 96);
        std::printf("  %-6s turned 90, 4:3   rms %5.2f  max %5.1f\n", b.name, e.rms, e.max);
        LUMEN_CHECK(e.rms <= 1.0 && e.max <= 2.0);
    }
}

// Where one bilinear tap already is the area filter — exactly 2:1 from whole texels, the
// render_scale = 2 on scale 1 case — the draw does not take it (no extra cost); at 4:3 and
// 2:1 from a half-texel origin it does.
LUMEN_TEST(exact_halving_needs_no_filter) {
    using lx::gfx::downsample_footprint_for;
    const auto xf = lx::buffer_transform::normal;
    LUMEN_CHECK(!downsample_footprint_for({0.f, 0.f, 200.f, 100.f}, {0, 0, 100, 50}, xf).active());
    LUMEN_CHECK(!downsample_footprint_for({0.f, 0.f, 100.f, 50.f}, {7, 3, 100, 50}, xf).active());
    LUMEN_CHECK(downsample_footprint_for({0.f, 0.f, 200.f, 100.f}, {0, 0, 150, 75}, xf).active());
    LUMEN_CHECK(downsample_footprint_for({0.5f, 0.f, 200.f, 100.f}, {0, 0, 100, 50}, xf).active());
    // Turned: the box's 300 texels span the 100-pixel height (3:1).
    const auto f = downsample_footprint_for({0.f, 0.f, 300.f, 100.f}, {0, 0, 100, 100},
                                            lx::buffer_transform::rotate_90);
    LUMEN_CHECK(f.u == 3.f && f.v == 1.f && f.active());
    // Magnification and slight shrinking (a rounded logical size) are not filtered.
    LUMEN_CHECK(!downsample_footprint_for({0.f, 0.f, 101.f, 100.f}, {0, 0, 100, 100}, xf).active());
    LUMEN_CHECK(!downsample_footprint_for({0.f, 0.f, 50.f, 50.f}, {0, 0, 100, 100}, xf).active());
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
