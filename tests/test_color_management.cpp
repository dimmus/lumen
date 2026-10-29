// Colour management (wp_color_manager_v1, lx.gfx color.cppm): every composite backend draws
// a described surface through the same transform, checked here against a reference computed
// in this file, in double precision, from the definitions alone — the RGB → XYZ matrices from
// the chromaticities (the classic x/y normalisation, not the backend's column form), Bradford,
// the sRGB, power and ST 2084 curves, the luminance anchor and the tone curve as documented.
//
// Each case composites a 128 × 128 texture of 256 cells — 216 opaque colours (every channel
// in {0, 32, 96, 160, 208, 255}) and 40 translucent greys — 1:1 onto an sRGB target and
// compares every pixel: CPU always, GL on a hardware render node (skipped without one),
// Vulkan on whatever device the selector finds (lavapipe here), plus a Vulkan and a GL UI
// stream frame. A surface described as plain sRGB must come out byte-identical to an
// undescribed one.
#include "lumen_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.scene;

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
constexpr auto k_argb = static_cast<lx::fourcc>(lx::pixel_format::argb8888);

// ── The reference ────────────────────────────────────────────────────────────────────

using mat3 = std::array<long double, 9>;

mat3 mul(const mat3& a, const mat3& b) {
    mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
    return r;
}

mat3 inverse(const mat3& m) {
    const long double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                            m[2] * (m[3] * m[7] - m[4] * m[6]);
    mat3 r{};
    r[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    r[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    r[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    r[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    r[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    r[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    r[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    r[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    r[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return r;
}

struct xy {
    long double rx, ry, gx, gy, bx, by, wx, wy;
};

constexpr xy k_bt709{0.64L, 0.33L, 0.30L, 0.60L, 0.15L, 0.06L, 0.3127L, 0.3290L};
constexpr xy k_bt2020{0.708L, 0.292L, 0.170L, 0.797L, 0.131L, 0.046L, 0.3127L, 0.3290L};
constexpr xy k_p3_d65{0.680L, 0.320L, 0.265L, 0.690L, 0.150L, 0.060L, 0.3127L, 0.3290L};
constexpr xy k_dci_p3{0.680L, 0.320L, 0.265L, 0.690L, 0.150L, 0.060L, 0.314L, 0.351L};

/// RGB → XYZ (SMPTE RP 177): primaries as XYZ with Y = 1, scaled so RGB = 1 is the white.
mat3 rgb_to_xyz(const xy& c) {
    const auto col = [](long double x, long double y) {
        return std::array<long double, 3>{x / y, 1.0L, (1.0L - x - y) / y};
    };
    const auto r = col(c.rx, c.ry), g = col(c.gx, c.gy), b = col(c.bx, c.by), w = col(c.wx, c.wy);
    const mat3 p{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const mat3 pi = inverse(p);
    long double s[3];
    for (int i = 0; i < 3; ++i)
        s[i] = pi[i * 3] * w[0] + pi[i * 3 + 1] * w[1] + pi[i * 3 + 2] * w[2];
    mat3 m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i * 3 + j] = p[i * 3 + j] * s[j];
    return m;
}

/// Source primaries → BT.709, with a Bradford adaptation to D65 when the whites differ.
mat3 to_bt709(const xy& src) {
    mat3 m = rgb_to_xyz(src);
    if (src.wx != k_bt709.wx || src.wy != k_bt709.wy) {
        const mat3 b{0.8951L, 0.2664L, -0.1614L, -0.7502L, 1.7135L, 0.0367L, 0.0389L, -0.0685L, 1.0296L};
        const auto lms = [&](long double x, long double y) {
            const long double X = x / y, Y = 1.0L, Z = (1.0L - x - y) / y;
            return std::array<long double, 3>{b[0] * X + b[1] * Y + b[2] * Z,
                                              b[3] * X + b[4] * Y + b[5] * Z,
                                              b[6] * X + b[7] * Y + b[8] * Z};
        };
        const auto s = lms(src.wx, src.wy), d = lms(k_bt709.wx, k_bt709.wy);
        const mat3 k{d[0] / s[0], 0, 0, 0, d[1] / s[1], 0, 0, 0, d[2] / s[2]};
        m = mul(mul(inverse(b), mul(k, b)), m);
    }
    return mul(inverse(rgb_to_xyz(k_bt709)), m);
}

long double srgb_decode(long double v) {
    return v <= 0.04045L ? v / 12.92L : std::pow((v + 0.055L) / 1.055L, 2.4L);
}
long double srgb_encode(long double v) {
    v = std::clamp(v, 0.0L, 1.0L);
    return v <= 0.0031308L ? v * 12.92L : 1.055L * std::pow(v, 1.0L / 2.4L) - 0.055L;
}
/// ST 2084 EOTF in cd/m².
long double pq_nits(long double v) {
    const long double m1 = 2610.0L / 16384.0L, m2 = 2523.0L / 4096.0L * 128.0L;
    const long double c1 = 3424.0L / 4096.0L, c2 = 2413.0L / 4096.0L * 32.0L,
                      c3 = 2392.0L / 4096.0L * 32.0L;
    const long double e = std::pow(v, 1.0L / m2);
    return 10000.0L * std::pow(std::max(e - c1, 0.0L) / (c2 - c3 * e), 1.0L / m1);
}

enum class tf { srgb, power, linear, pq };

/// One test description, as the reference sees it, and as the compositor is given it.
struct desc {
    const char* name;
    tf curve;
    long double power;
    xy prim;
    long double min, max, ref;
    long double max_cll; // 0: unset
};

lx::gfx::color_description to_gfx(const desc& d) {
    lx::gfx::color_description g{};
    g.tf = d.curve == tf::srgb    ? lx::gfx::color_tf::srgb
           : d.curve == tf::power ? lx::gfx::color_tf::power
           : d.curve == tf::linear ? lx::gfx::color_tf::ext_linear
                                   : lx::gfx::color_tf::st2084_pq;
    g.power = static_cast<float>(d.power);
    g.primaries = {static_cast<float>(d.prim.rx), static_cast<float>(d.prim.ry),
                   static_cast<float>(d.prim.gx), static_cast<float>(d.prim.gy),
                   static_cast<float>(d.prim.bx), static_cast<float>(d.prim.by),
                   static_cast<float>(d.prim.wx), static_cast<float>(d.prim.wy)};
    g.min_lum = static_cast<float>(d.min);
    g.max_lum = static_cast<float>(d.max);
    g.ref_lum = static_cast<float>(d.ref);
    g.max_cll = static_cast<float>(d.max_cll);
    return g;
}

/// The reference's working-space light (BT.709, 1 = the output's white) for one straight
/// encoded colour, tone mapped. Anchor: (L − min) / (ref − min), L = min + (max − min)·E
/// (PQ: the absolute EOTF). Peak: max_cll or the maximum, anchored, capped by the signal.
std::array<long double, 3> reference_light(const desc& d, const std::array<long double, 3>& v) {
    // The float chromaticities the compositor is given are what it converts from.
    const lx::gfx::color_description g = to_gfx(d);
    const xy p{g.primaries.rx, g.primaries.ry, g.primaries.gx, g.primaries.gy,
               g.primaries.bx, g.primaries.by, g.primaries.wx, g.primaries.wy};
    const mat3 m = to_bt709(p);
    const long double min = g.min_lum, ref = g.ref_lum;
    const long double max = d.curve == tf::pq ? min + 10000.0L : static_cast<long double>(g.max_lum);
    std::array<long double, 3> rel{};
    for (int i = 0; i < 3; ++i) {
        long double l = 0; // L − min
        switch (d.curve) {
        case tf::srgb: l = (max - min) * srgb_decode(v[i]); break;
        case tf::power: l = (max - min) * std::pow(v[i], static_cast<long double>(g.power)); break;
        case tf::linear: l = (max - min) * v[i]; break;
        case tf::pq: l = pq_nits(v[i]); break;
        }
        rel[i] = l / (ref - min);
    }
    std::array<long double, 3> out{};
    for (int i = 0; i < 3; ++i)
        out[i] = m[i * 3] * rel[0] + m[i * 3 + 1] * rel[1] + m[i * 3 + 2] * rel[2];
    const long double cap = d.curve == tf::pq ? 10000.0L / (ref - min) : (max - min) / (ref - min);
    long double peak = ((d.max_cll > 0 ? static_cast<long double>(g.max_cll) : max) - min) / (ref - min);
    if (d.curve != tf::linear)
        peak = std::min(peak, cap);
    if (peak > 1.0L) {
        const long double k = 0.8L;
        const long double x = std::max({out[0], out[1], out[2]});
        if (x > k) {
            const long double w = (peak - k) / (1 - k), t = (x - k) / (1 - k);
            const long double y = k + (1 - k) * (t * (1 + t / (w * w)) / (1 + t));
            for (long double& c : out)
                c *= y / x;
        }
    }
    return out;
}

// ── The test image ───────────────────────────────────────────────────────────────────

constexpr int k_size = 128;
constexpr int k_cell = 8;
constexpr unsigned k_levels[6] = {0, 32, 96, 160, 208, 255};

struct texel {
    unsigned r, g, b, a; // premultiplied
};

texel cell_texel(int cell) {
    if (cell < 216)
        return {k_levels[cell / 36], k_levels[(cell / 6) % 6], k_levels[cell % 6], 255};
    // Translucent greys: 5 alphas × 8 levels.
    const int i = cell - 216;
    const unsigned a = 51u * static_cast<unsigned>(1 + i / 8);
    const unsigned g = static_cast<unsigned>(std::lround(32.0 * (i % 8) * a / 255.0));
    return {g, g, g, a};
}

/// RGBA bytes (premultiplied), cells row-major.
std::vector<unsigned char> test_image() {
    std::vector<unsigned char> px(static_cast<std::size_t>(k_size) * k_size * 4);
    for (int y = 0; y < k_size; ++y)
        for (int x = 0; x < k_size; ++x) {
            const texel t = cell_texel((y / k_cell) * (k_size / k_cell) + x / k_cell);
            unsigned char* p = &px[(static_cast<std::size_t>(y) * k_size + x) * 4];
            p[0] = static_cast<unsigned char>(t.r);
            p[1] = static_cast<unsigned char>(t.g);
            p[2] = static_cast<unsigned char>(t.b);
            p[3] = static_cast<unsigned char>(t.a);
        }
    return px;
}

/// How a backend lands a translucent texel over the opaque black clear: Vulkan blends linear
/// light (encode(light · α)); GL and CPU blend encoded values (encode(light) · α).
enum class blend_space { linear, encoded };

/// The expected 8-bit value of channel `c` of cell `cell` under `d` (null: undescribed sRGB).
int expected(const desc* d, int cell, int c, blend_space space) {
    const texel t = cell_texel(cell);
    const unsigned ch[3] = {t.r, t.g, t.b};
    const long double a = t.a / 255.0L;
    if (!d) // plain sRGB: decoded, blended, encoded — over black the texel itself
        return static_cast<int>(space == blend_space::linear
                                    ? std::lround(srgb_encode(srgb_decode(ch[c] / 255.0L / a) * a) * 255.0L)
                                    : static_cast<long>(ch[c]));
    const std::array<long double, 3> v{ch[0] / static_cast<long double>(t.a),
                                       ch[1] / static_cast<long double>(t.a),
                                       ch[2] / static_cast<long double>(t.a)};
    const auto light = reference_light(*d, v);
    const long double enc = space == blend_space::linear ? srgb_encode(light[c] * a)
                                                         : srgb_encode(light[c]) * a;
    return static_cast<int>(std::lround(enc * 255.0L));
}

struct result {
    int worst = 0;
    unsigned compared = 0;
};

/// `rgba` (RGBA rows of the target) against `expected`.
result compare(const std::vector<unsigned char>& rgba, const desc* d, blend_space space) {
    result r{};
    for (int y = 0; y < k_size; ++y)
        for (int x = 0; x < k_size; ++x) {
            const int cell = (y / k_cell) * (k_size / k_cell) + x / k_cell;
            for (int c = 0; c < 3; ++c) {
                const int got = rgba[(static_cast<std::size_t>(y) * k_size + x) * 4 + c];
                r.worst = std::max(r.worst, std::abs(got - expected(d, cell, c, space)));
                ++r.compared;
            }
        }
    return r;
}

// PQ BT.2020 (HDR10's encoding) at its defaults (0.005 / 10 000 / 203): highlights up to
// 10 000 cd/m², tone mapped to the output's white. Then with content light up to 1000 cd/m²
// (max_cll), an SDR signal in a PQ container (max_cll = reference white: no tone mapping),
// gamma 2.2, extended linear and the sRGB curve on BT.2020, a pure power 2.6 on DCI-P3
// (its own white: Bradford), and gamma 2.2 Display P3 with headroom of its own (200 / 400).
const desc k_cases[] = {
    {"PQ BT.2020", tf::pq, 0, k_bt2020, 0.005L, 10000.L, 203.L, 0},
    {"PQ BT.2020 max_cll 1000", tf::pq, 0, k_bt2020, 0.005L, 10000.L, 203.L, 1000.L},
    {"PQ sRGB max_cll 203", tf::pq, 0, k_bt709, 0.005L, 10000.L, 203.L, 203.L},
    {"gamma 2.2 sRGB", tf::power, 2.2L, k_bt709, 0.2L, 80.L, 80.L, 0},
    {"ext_linear sRGB", tf::linear, 0, k_bt709, 0.2L, 80.L, 80.L, 0},
    {"sRGB curve BT.2020", tf::srgb, 0, k_bt2020, 0.2L, 80.L, 80.L, 0},
    {"power 2.6 DCI-P3", tf::power, 2.6L, k_dci_p3, 0.2L, 80.L, 80.L, 0},
    {"gamma 2.2 Display P3 0/400/200", tf::power, 2.2L, k_p3_d65, 0.L, 400.L, 200.L, 0},
};

lx::gfx::blit_command image_draw(const desc* d) {
    lx::gfx::blit_command cmd{};
    cmd.texture_id = 1;
    cmd.dst = {0, 0, k_size, k_size};
    cmd.blend = lx::blend_mode::premultiplied;
    if (d)
        LUMEN_CHECK(lx::gfx::make_color_transform(to_gfx(*d), cmd.color));
    return cmd;
}

/// Runs `composite(cmd) → RGBA rows` for every case and the off-path, checking each.
template <typename Fn>
void check_backend(const char* backend, blend_space space, int tolerance, Fn composite) {
    const std::vector<unsigned char> plain = composite(image_draw(nullptr));
    LUMEN_CHECK(!plain.empty());
    if (plain.empty())
        return;
    const result base = compare(plain, nullptr, space);
    std::printf("  %s undescribed: worst %d over %u values\n", backend, base.worst, base.compared);
    LUMEN_CHECK(base.worst <= tolerance);
    // Off path: a description that works out as sRGB draws the very same bytes.
    lx::gfx::blit_command srgb = image_draw(nullptr);
    LUMEN_CHECK(lx::gfx::make_color_transform(lx::gfx::k_srgb_description, srgb.color));
    LUMEN_CHECK(!srgb.color.active);
    LUMEN_CHECK(composite(srgb) == plain);
    for (const desc& d : k_cases) {
        const std::vector<unsigned char> got = composite(image_draw(&d));
        LUMEN_CHECK(!got.empty());
        if (got.empty())
            continue;
        const result r = compare(got, &d, space);
        std::printf("  %s %-32s worst %d over %u values\n", backend, d.name, r.worst, r.compared);
        LUMEN_CHECK(r.worst <= tolerance);
        LUMEN_CHECK(got != plain);
    }
}

} // namespace

// ── The transform itself ─────────────────────────────────────────────────────────────

LUMEN_TEST(matrices_match_the_reference_and_published_values) {
    // ITU-R BT.2087-0 (BT.2020 → BT.709, linear light), to its four decimals.
    const double bt2087[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083,
                              -0.0182, -0.1006, 1.1187};
    double m[9];
    LUMEN_CHECK(lx::gfx::primaries_to_bt709(lx::gfx::primaries_of(lx::gfx::named_primaries::bt2020), m));
    for (int i = 0; i < 9; ++i)
        LUMEN_CHECK(std::fabs(m[i] - bt2087[i]) < 1e-4);
    // Every case's matrix against the reference's, and sRGB → identity.
    for (const desc& d : k_cases) {
        const lx::gfx::color_description g = to_gfx(d);
        const mat3 r = to_bt709({g.primaries.rx, g.primaries.ry, g.primaries.gx, g.primaries.gy,
                                 g.primaries.bx, g.primaries.by, g.primaries.wx, g.primaries.wy});
        LUMEN_CHECK(lx::gfx::primaries_to_bt709(g.primaries, m));
        double worst = 0;
        for (int i = 0; i < 9; ++i)
            worst = std::max(worst, std::fabs(m[i] - static_cast<double>(r[i])));
        std::printf("  %-32s matrix differs from the reference by %.2e\n", d.name, worst);
        // The compositor's own BT.709 is float chromaticities (0.64f …), the reference's exact
        // decimals: they differ by ~1e-7, far below an 8-bit level (4e-3).
        LUMEN_CHECK(worst < 1e-6);
    }
    LUMEN_CHECK(lx::gfx::primaries_to_bt709({}, m));
    for (int i = 0; i < 9; ++i)
        LUMEN_CHECK(std::fabs(m[i] - (i % 4 == 0 ? 1.0 : 0.0)) < 1e-12);
    // Collinear primaries, a white at y = 0: no matrix.
    LUMEN_CHECK(!lx::gfx::primaries_to_bt709({0.1f, 0.1f, 0.2f, 0.2f, 0.3f, 0.3f, 0.3127f, 0.329f}, m));
    LUMEN_CHECK(!lx::gfx::primaries_to_bt709({0.64f, 0.33f, 0.3f, 0.6f, 0.15f, 0.06f, 0.3f, 0.f}, m));
    // CIE 1931 XYZ's own primaries (y = 0 for red and blue) still have one.
    LUMEN_CHECK(lx::gfx::primaries_to_bt709(lx::gfx::primaries_of(lx::gfx::named_primaries::cie1931_xyz), m));
}

LUMEN_TEST(transform_anchors_reference_white_and_finds_the_peak) {
    lx::gfx::color_transform t{};
    LUMEN_CHECK(lx::gfx::make_color_transform(lx::gfx::k_srgb_description, t));
    LUMEN_CHECK(!t.active);
    // PQ at its defaults: 203 cd/m² is 1.0, the peak 10 000 cd/m² → 9999.995 / 202.995.
    lx::gfx::color_description pq{};
    pq.tf = lx::gfx::color_tf::st2084_pq;
    pq.primaries = lx::gfx::primaries_of(lx::gfx::named_primaries::bt2020);
    lx::gfx::set_default_luminances(pq);
    LUMEN_CHECK(lx::gfx::make_color_transform(pq, t));
    LUMEN_CHECK(t.active && t.tf == lx::gfx::color_tf::st2084_pq);
    const double s = 10000.0 / (203.0 - 0.005);
    LUMEN_CHECK(std::fabs(t.matrix[0] - 1.6605 * s) < 1e-3 * s);
    LUMEN_CHECK(std::fabs(t.peak - s) < 1e-3);
    // PQ 203 cd/m² on BT.709: (203 − 0.005) / (203 − 0.005) = 1.0 before tone mapping —
    // with content up to 10 000 cd/m² the curve then puts reference white at 0.9, the
    // knee plus half the room above it (t = 1: f = (1 + 1/w²) / 2).
    pq.primaries = {};
    LUMEN_CHECK(lx::gfx::make_color_transform(pq, t));
    float v[3];
    const float e = lx::linear_to_pq((203.f - 0.005f) / 10000.f);
    const float in[3] = {e, e, e};
    lx::gfx::apply_color_transform(t, in, v);
    const double w = (s - 0.8) / 0.2;
    for (float c : v)
        LUMEN_CHECK(std::fabs(c - (0.8 + 0.2 * (1.0 + 1.0 / (w * w)) / 2.0)) < 1e-3);
    // max_cll at reference white: no headroom, no tone mapping — 203 cd/m² is 1.0.
    pq.max_cll = 203.f;
    LUMEN_CHECK(lx::gfx::make_color_transform(pq, t));
    LUMEN_CHECK(t.peak == 1.f);
    lx::gfx::apply_color_transform(t, in, v);
    for (float c : v)
        LUMEN_CHECK(std::fabs(c - 1.f) < 1e-3f);
    // Degenerate luminances.
    lx::gfx::color_description bad{};
    bad.ref_lum = 0.1f;
    LUMEN_CHECK(!lx::gfx::make_color_transform(bad, t));
}

LUMEN_TEST(tone_curve_is_continuous_monotonic_and_lands_the_peak_on_white) {
    for (const float peak : {1.05f, 2.f, 4.926f, 49.26f}) {
        LUMEN_CHECK(lx::gfx::tone_map_scale(0.5f, peak) == 1.f);
        LUMEN_CHECK(lx::gfx::tone_map_scale(lx::gfx::k_tone_knee, peak) == 1.f);
        // Slope 1 at the knee.
        const float h = 1e-3f;
        const float x = lx::gfx::k_tone_knee + h;
        LUMEN_CHECK(std::fabs(x * lx::gfx::tone_map_scale(x, peak) - x) < 2e-5f * peak);
        float last = 0.f;
        for (float v = 0.f; v <= peak; v += peak / 997.f) {
            const float y = v * lx::gfx::tone_map_scale(v, peak);
            LUMEN_CHECK(y >= last - 1e-6f);
            LUMEN_CHECK(y <= 1.f + 1e-6f);
            last = y;
        }
        LUMEN_CHECK(std::fabs(peak * lx::gfx::tone_map_scale(peak, peak) - 1.f) < 1e-5f);
    }
    LUMEN_CHECK(lx::gfx::tone_map_scale(5.f, 1.f) == 1.f); // no headroom: nothing to do
}

// ── Backends ─────────────────────────────────────────────────────────────────────────

LUMEN_TEST(cpu_draws_each_description_as_the_reference) {
    // The CPU path takes ARGB words: B, G, R, A in memory.
    const std::vector<unsigned char> rgba = test_image();
    std::vector<std::uint32_t> src(static_cast<std::size_t>(k_size) * k_size);
    for (std::size_t i = 0; i < src.size(); ++i)
        src[i] = (std::uint32_t{rgba[i * 4 + 3]} << 24) | (std::uint32_t{rgba[i * 4]} << 16) |
                 (std::uint32_t{rgba[i * 4 + 1]} << 8) | rgba[i * 4 + 2];
    lx::gfx::pixel_surface s{};
    s.pixels = reinterpret_cast<unsigned char*>(src.data());
    s.width = s.height = k_size;
    s.stride = k_size * 4;
    s.format = k_argb;
    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, s)));
    check_backend("CPU", blend_space::encoded, 1, [&](lx::gfx::blit_command cmd) {
        std::vector<std::uint32_t> dst(static_cast<std::size_t>(k_size) * k_size, 0xDEADBEEFu);
        lx::gfx::pixel_surface d = s;
        d.pixels = reinterpret_cast<unsigned char*>(dst.data());
        d.format = k_xrgb;
        auto st = comp.composite(d, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
        std::vector<unsigned char> out;
        if (!st)
            return out;
        out.resize(dst.size() * 4);
        for (std::size_t i = 0; i < dst.size(); ++i) {
            out[i * 4] = static_cast<unsigned char>(dst[i] >> 16);
            out[i * 4 + 1] = static_cast<unsigned char>(dst[i] >> 8);
            out[i * 4 + 2] = static_cast<unsigned char>(dst[i]);
            out[i * 4 + 3] = 255;
        }
        return out;
    });
}

LUMEN_TEST(cpu_converts_a_described_solid_colour) {
    // A single-pixel buffer with a description: its colour goes through the transform once.
    std::vector<std::uint32_t> dst(16, 0u);
    lx::gfx::pixel_surface d{};
    d.pixels = reinterpret_cast<unsigned char*>(dst.data());
    d.width = d.height = 4;
    d.stride = 16;
    d.format = k_xrgb;
    lx::gfx::cpu_compositor comp;
    lx::gfx::blit_command cmd{};
    cmd.solid = true;
    cmd.solid_color = lx::color::rgb(0.6f, 0.3f, 0.1f, 1.f);
    cmd.dst = {0, 0, 4, 4};
    cmd.blend = lx::blend_mode::opaque;
    LUMEN_CHECK(lx::gfx::make_color_transform(to_gfx(k_cases[5]), cmd.color));
    LUMEN_CHECK(static_cast<bool>(comp.composite(d, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {})));
    const auto light = reference_light(k_cases[5], {0.6L, 0.3L, 0.1L});
    const int want[3] = {static_cast<int>(std::lround(srgb_encode(light[0]) * 255.0L)),
                         static_cast<int>(std::lround(srgb_encode(light[1]) * 255.0L)),
                         static_cast<int>(std::lround(srgb_encode(light[2]) * 255.0L))};
    const int got[3] = {static_cast<int>((dst[5] >> 16) & 0xFF), static_cast<int>((dst[5] >> 8) & 0xFF),
                        static_cast<int>(dst[5] & 0xFF)};
    for (int c = 0; c < 3; ++c)
        LUMEN_CHECK(std::abs(got[c] - want[c]) <= 1);
}

namespace {

lx::gfx::egl_device* gl_device() {
    static lx::gfx::egl_device device{};
    static bool initialized = false;
    static bool usable = false;
    if (!initialized) {
        initialized = true;
        const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0)
            return nullptr;
        auto created = lx::gfx::egl_device::create(fd);
        if (!created) {
            std::printf("  (skip: %s)\n", created.get_error().message);
            return nullptr;
        }
        device = std::move(created).value();
        if (device.is_software_renderer()) {
            std::printf("  (skip: GL renderer is software — %s)\n", device.renderer());
            return nullptr;
        }
        usable = true;
    }
    return usable ? &device : nullptr;
}

struct vk_fixture {
    lx::gfx::device device{};
    lx::gfx::vulkan_compositor compositor{};
};

/// Torn down by `main`: the loader and the validation layer are gone by static destruction.
std::unique_ptr<vk_fixture> g_vk{};
std::string g_vk_validation_log{};

vk_fixture* vk() {
    static bool tried = false;
    if (!tried) {
        tried = true;
        auto selected = lx::gfx::device_selector::select_best();
        if (!selected || !selected.value().info().supports_dmabuf_import) {
            std::printf("  (skip: no Vulkan device)\n");
            return nullptr;
        }
        auto f = std::make_unique<vk_fixture>();
        f->device = std::move(selected).value();
        if (auto ready = f->compositor.initialize(f->device.context()); !ready) {
            std::printf("  (skip: %s)\n", ready.get_error().message);
            return nullptr;
        }
        g_vk = std::move(f);
    }
    return g_vk.get();
}

/// A UI stream frame: one quad primitive (outside the image, so the frame is a stream) and
/// the image as a surface ref.
struct stream_frame {
    lx::scene::draw_list list{};
    std::vector<lx::gfx::blit_command> blits;
    lx::gfx::ui_stream stream{};
};

void build_stream(stream_frame& f, const lx::gfx::blit_command& image, int w, int h) {
    lx::scene::draw_list& l = f.list;
    l.begin({w, h});
    lx::scene::draw_command back{};
    back.texture = lx::texture_id{1};
    back.dst = image.dst;
    back.blend = image.blend;
    back.color = image.color;
    back.sort_key = lx::draw_sort_key::make(0, l.size());
    l.push(back);
    const lx::rect2i square{k_size + 4, 4, 16, 16};
    lx::gfx::quad_instance q{};
    q.bounds[0] = static_cast<float>(square.x);
    q.bounds[1] = static_cast<float>(square.y);
    q.bounds[2] = static_cast<float>(square.width);
    q.bounds[3] = static_cast<float>(square.height);
    const lx::gfx::packed_rgba bg = lx::gfx::premultiplied_half(lx::color::rgb(1.f, 0.f, 0.f));
    q.background[0] = q.background_end[0] = bg.r;
    q.background[1] = q.background_end[1] = bg.g;
    q.background[2] = q.background_end[2] = bg.b;
    q.background[3] = q.background_end[3] = bg.a;
    l.push_quad(q, square, l.orders().assign(square));
    l.batch_for_render();
    f.blits.clear();
    for (unsigned i = 0; i < l.size(); ++i) {
        const lx::scene::draw_command& d = l.data()[i];
        lx::gfx::blit_command b{};
        b.texture_id = d.texture.id();
        b.dst = d.dst;
        b.blend = d.blend;
        b.color = d.color;
        f.blits.push_back(b);
    }
    f.stream.refs = l.refs();
    f.stream.batches = l.batches();
    f.stream.quads = l.quad_instances();
    f.stream.surfaces = {f.blits.data(), f.blits.size()};
}

/// The image's square of a wider target's RGBA rows.
std::vector<unsigned char> crop(const std::vector<unsigned char>& rows, int w) {
    std::vector<unsigned char> out(static_cast<std::size_t>(k_size) * k_size * 4);
    for (int y = 0; y < k_size; ++y)
        std::memcpy(&out[static_cast<std::size_t>(y) * k_size * 4],
                    &rows[static_cast<std::size_t>(y) * w * 4], static_cast<std::size_t>(k_size) * 4);
    return out;
}

} // namespace

LUMEN_TEST(gl_draws_each_description_as_the_reference) {
    auto* dev = gl_device();
    if (!dev)
        return;
    LUMEN_CHECK(static_cast<bool>(dev->make_current()));
    lx::gfx::gl_compositor comp{};
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*dev)));
    if (!comp.supports_color_management()) {
        std::printf("  (skip: no fragment highp — colour management not offered on GL)\n");
        return;
    }
    const std::vector<unsigned char> px = test_image();
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, k_size, k_size, px.data())));
    auto made = lx::gfx::gl_scanout_target::create(*dev, k_size, k_size, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::gl_scanout_target target = std::move(made).value();
    check_backend("GL", blend_space::encoded, 1, [&](lx::gfx::blit_command cmd) {
        std::vector<unsigned char> out;
        auto c = comp.composite(target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
        if (!c)
            return out;
        if (c.value().out_fence_fd >= 0)
            ::close(c.value().out_fence_fd);
        out.resize(static_cast<std::size_t>(k_size) * k_size * 4);
        if (!comp.read_back(target, out.data(), static_cast<unsigned>(out.size())))
            out.clear();
        return out;
    });
    comp.shutdown();
}

LUMEN_TEST(gl_stream_frame_draws_a_described_surface) {
    auto* dev = gl_device();
    if (!dev || !dev->gles_at_least(3, 1))
        return;
    LUMEN_CHECK(static_cast<bool>(dev->make_current()));
    lx::gfx::gl_compositor comp{};
    lx::gfx::ui_gl_renderer ui{};
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*dev)));
    LUMEN_CHECK(static_cast<bool>(ui.initialize()));
    if (!comp.supports_color_management())
        return;
    ui.set_surface_compositor(&comp);
    const int w = k_size + 32, h = k_size;
    auto made = lx::gfx::ui_gl_target::create(w, h);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::ui_gl_target target = std::move(made).value();
    const std::vector<unsigned char> px = test_image();
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, k_size, k_size, px.data())));
    const desc& d = k_cases[0];
    stream_frame f{};
    build_stream(f, image_draw(&d), w, h);
    LUMEN_CHECK(f.list.primitive_count() == 1 && !f.stream.batches.empty());
    lx::gfx::damage_region dmg{};
    dmg.set_full();
    auto drawn = ui.draw(target.framebuffer(), w, h, f.stream, nullptr, dmg, lx::color::rgb(0.f, 0.f, 0.f));
    LUMEN_CHECK(static_cast<bool>(drawn));
    auto fence = comp.fence_frame();
    if (fence && fence.value() >= 0)
        ::close(fence.value());
    std::vector<std::uint32_t> words(static_cast<std::size_t>(w) * h);
    LUMEN_CHECK(static_cast<bool>(target.read_back(words)));
    std::vector<unsigned char> rows(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        rows[i * 4] = static_cast<unsigned char>(words[i] >> 16);
        rows[i * 4 + 1] = static_cast<unsigned char>(words[i] >> 8);
        rows[i * 4 + 2] = static_cast<unsigned char>(words[i]);
        rows[i * 4 + 3] = 255;
    }
    const result r = compare(crop(rows, w), &d, blend_space::encoded);
    std::printf("  GL stream %s: worst %d over %u values\n", d.name, r.worst, r.compared);
    LUMEN_CHECK(r.worst <= 1);
    ui.shutdown();
    comp.shutdown();
}

LUMEN_TEST(vulkan_draws_each_description_as_the_reference) {
    vk_fixture* v = vk();
    if (!v)
        return;
    if (!v->compositor.supports_color_management()) {
        std::printf("  (skip: the device has no room for the colour push block)\n");
        return;
    }
    const std::vector<unsigned char> px = test_image();
    LUMEN_CHECK(static_cast<bool>(v->compositor.upload_rgba(1, k_size, k_size, px.data())));
    auto made = lx::gfx::render_target::create(v->device.context(), k_size, k_size);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::render_target target = std::move(made).value();
    // The FP16 intermediate holds light to 11 bits: one level, as for undescribed draws.
    check_backend("Vulkan", blend_space::linear, 1, [&](lx::gfx::blit_command cmd) {
        std::vector<unsigned char> out;
        auto c = v->compositor.composite(target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
        if (!c)
            return out;
        if (c.value().out_fence_fd >= 0)
            ::close(c.value().out_fence_fd);
        out.resize(static_cast<std::size_t>(k_size) * k_size * 4);
        if (!v->compositor.read_back(target, out.data(), static_cast<unsigned>(out.size())))
            out.clear();
        return out;
    });
    v->compositor.forget_texture(1);
}

LUMEN_TEST(vulkan_stream_frame_draws_a_described_surface) {
    vk_fixture* v = vk();
    if (!v || !v->compositor.supports_color_management())
        return;
    lx::gfx::ui_vk_renderer ui{};
    if (auto ok = ui.initialize(v->device.context()); !ok) {
        std::printf("  (skip: %s)\n", ok.get_error().message);
        return;
    }
    const int w = k_size + 32, h = k_size;
    auto made = lx::gfx::render_target::create(v->device.context(), w, h);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::render_target target = std::move(made).value();
    const std::vector<unsigned char> px = test_image();
    LUMEN_CHECK(static_cast<bool>(v->compositor.upload_rgba(1, k_size, k_size, px.data())));
    for (const desc* d : {&k_cases[0], &k_cases[6]}) {
        stream_frame f{};
        build_stream(f, image_draw(d), w, h);
        auto c = ui.composite(v->compositor, target, lx::color::rgb(0.f, 0.f, 0.f), f.stream, nullptr);
        LUMEN_CHECK(static_cast<bool>(c));
        if (!c)
            return;
        if (c->composite.out_fence_fd >= 0)
            ::close(c->composite.out_fence_fd);
        std::vector<unsigned char> rows(static_cast<std::size_t>(w) * h * 4);
        LUMEN_CHECK(static_cast<bool>(
            v->compositor.read_back(target, rows.data(), static_cast<unsigned>(rows.size()))));
        const result r = compare(crop(rows, w), d, blend_space::linear);
        std::printf("  Vulkan stream %s: worst %d over %u values\n", d->name, r.worst, r.compared);
        LUMEN_CHECK(r.worst <= 1);
    }
    v->compositor.forget_texture(1);
    ui.shutdown();
}

LUMEN_TEST(scene_carries_the_surface_transform_to_its_draws) {
    lx::scene::surface_node node{lx::surface_id{7}};
    lx::gfx::imported_image img{};
    img.image_id = 3;
    img.width = 16;
    img.height = 16;
    node.attach(img);
    node.set_bounds({0, 0, 16, 16});
    lx::gfx::color_transform t{};
    LUMEN_CHECK(lx::gfx::make_color_transform(to_gfx(k_cases[0]), t));
    node.set_color(t);
    lx::scene::draw_list list{};
    list.begin({64, 64});
    node.emit_draws(list, lx::scene::emit_context{});
    LUMEN_CHECK(list.size() == 1);
    LUMEN_CHECK(list.size() == 1 && list.data()[0].color == t);
    node.set_color({});
    list.begin({64, 64});
    node.emit_draws(list, lx::scene::emit_context{});
    LUMEN_CHECK(list.size() == 1 && !list.data()[0].color.active);
}

LUMEN_TEST(vulkan_validation_clean) {
    if (g_vk_validation_log.empty())
        return;
    std::FILE* f = std::fopen(g_vk_validation_log.c_str(), "r");
    unsigned lines = 0;
    char line[2048];
    while (f && std::fgets(line, sizeof line, f)) {
        if (lines < 5)
            std::printf("    %s", line);
        ++lines;
    }
    if (f)
        std::fclose(f);
    std::printf("    validation layer log: %u lines\n", lines);
    LUMEN_CHECK(lines == 0);
}

int main(int argc, char** argv) {
    if (const char* v = std::getenv("LUMEN_VULKAN_VALIDATION"); v && *v == '1') {
        const char* found = std::getenv("LUMEN_VK_VALIDATION_LAYER");
        const std::string manifest =
            found && *found ? found
                            : "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json";
        if (::access(manifest.c_str(), R_OK) == 0) {
            g_vk_validation_log =
                "/tmp/lumen-color-vk-validation-" + std::to_string(::getpid()) + ".log";
            ::setenv("VK_ADD_LAYER_PATH", manifest.substr(0, manifest.find_last_of('/')).c_str(), 1);
            ::setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
            ::setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG", 1);
            ::setenv("VK_KHRONOS_VALIDATION_LOG_FILENAME", g_vk_validation_log.c_str(), 1);
            ::setenv("VK_KHRONOS_VALIDATION_REPORT_FLAGS", "error,warn", 1);
            std::remove(g_vk_validation_log.c_str());
        }
    }
    const int status = lumen_test::run_all(argc, argv);
    g_vk.reset();
    if (!g_vk_validation_log.empty())
        std::remove(g_vk_validation_log.c_str());
    return status;
}
