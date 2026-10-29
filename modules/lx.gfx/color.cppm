module;

import lx.foundation;

export module lx.gfx:color;

import std;

// ── Colour management (wp_color_manager_v1) ────────────────────────────────────
//
// A client may say what its pixels mean: a parametric image description (transfer
// function, primaries, luminances). Every composite backend draws such a surface through one
// per-draw transform into its working space, so the three backends agree:
//
//   working space   linear light, BT.709 (sRGB) primaries, D65 white, 1.0 = the output's
//                   reference white (SDR white). What the Vulkan intermediate holds and what
//                   GL and CPU decode to before they encode again. Unbounded: negative
//                   values are colours outside BT.709, values above 1 are highlights.
//
//   per draw        E   decode the transfer function to normalized light (PQ: 1 = 10 000
//                       cd/m², ext_linear unbounded, power curves mirrored through 0)
//                   M   3 × 3 matrix, linear light: the source primaries to XYZ, Bradford
//                       from the source white to D65, XYZ to BT.709 — times the luminance
//                       anchor s (below)
//                   T   tone map when the source can be brighter than the output's white
//                       (`color_transform::peak` > 1): `tone_map_scale`
//
//   anchor s        the description's reference white lands on the output's:
//                       PQ:     s = 10 000 / (ref − min)       (L = min + 10 000 · E)
//                       others: s = (max − min) / (ref − min)  (L = min + (max − min) · E)
//                   so working = (L − min) / (ref − min). With the sRGB defaults (0.2, 80,
//                   80) s = 1; PQ's defaults (0.005, 10 000, 203) put 203 cd/m² at 1.0.
//
//   peak            the brightest working value the content holds: from max_cll, else the
//                   mastering (target) maximum, else the primary maximum, anchored the same
//                   way and capped by what the signal can encode. ≤ 1: no tone mapping.
//
// A surface without a description, or one whose transform works out as the identity (sRGB
// decode, identity matrix, no headroom), has an inactive transform and is drawn exactly as
// before: no extra pass, no other shader, the same pixels.

export namespace lx::gfx {

/// CIE 1931 xy of the three primaries and the white point.
struct chromaticities {
    float rx = 0.640f;
    float ry = 0.330f;
    float gx = 0.300f;
    float gy = 0.600f;
    float bx = 0.150f;
    float by = 0.060f;
    float wx = 0.3127f;
    float wy = 0.3290f;

    [[nodiscard]] friend constexpr bool operator==(const chromaticities&,
                                                   const chromaticities&) = default;
};

/// wp_color_manager_v1.primaries values the compositor names (all of version 1's).
enum class named_primaries : unsigned char {
    srgb = 1,
    pal_m = 2,
    pal = 3,
    ntsc = 4,
    generic_film = 5,
    bt2020 = 6,
    cie1931_xyz = 7,
    dci_p3 = 8,
    display_p3 = 9,
    adobe_rgb = 10,
};

/// The chromaticities of a named set (H.273 / the protocol's references).
[[nodiscard]] constexpr chromaticities primaries_of(named_primaries p) {
    switch (p) {
    case named_primaries::pal_m: return {0.67f, 0.33f, 0.21f, 0.71f, 0.14f, 0.08f, 0.310f, 0.316f};
    case named_primaries::pal: return {0.64f, 0.33f, 0.29f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f};
    case named_primaries::ntsc:
        return {0.630f, 0.340f, 0.310f, 0.595f, 0.155f, 0.070f, 0.3127f, 0.3290f};
    case named_primaries::generic_film:
        return {0.681f, 0.319f, 0.243f, 0.692f, 0.145f, 0.049f, 0.310f, 0.316f};
    case named_primaries::bt2020:
        return {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
    case named_primaries::cie1931_xyz:
        return {1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f / 3.f, 1.f / 3.f};
    case named_primaries::dci_p3:
        return {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.314f, 0.351f};
    case named_primaries::display_p3:
        return {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f};
    case named_primaries::adobe_rgb:
        return {0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f, 0.3127f, 0.3290f};
    case named_primaries::srgb:
    default: return {};
    }
}

/// Transfer characteristics the composite decodes.
enum class color_tf : unsigned char {
    /// IEC 61966-2-1 piecewise sRGB (protocol `srgb` in version 1, `compound_power_2_4`).
    srgb,
    /// A pure power curve E = V^power, mirrored for negative V (`gamma22` is 2.2,
    /// `set_tf_power` any exponent in [1, 10]).
    power,
    /// `ext_linear`: E = V over all reals.
    ext_linear,
    /// SMPTE ST 2084: E in units of 10 000 cd/m².
    st2084_pq,
};

/// One parametric image description: what the client's pixel values mean. Luminances in
/// cd/m². A target value ≤ 0 is unset (the primary volume's value applies); max_cll and
/// max_fall 0 are unknown.
struct color_description {
    color_tf tf = color_tf::srgb;
    float power = 2.2f;
    chromaticities primaries{};
    float min_lum = 0.2f;
    float max_lum = 80.f;
    float ref_lum = 80.f;
    bool has_target_primaries = false;
    chromaticities target_primaries{};
    float target_min_lum = 0.f;
    float target_max_lum = 0.f;
    float max_cll = 0.f;
    float max_fall = 0.f;

    [[nodiscard]] friend constexpr bool operator==(const color_description&,
                                                   const color_description&) = default;
};

/// What the output encodes for (every output today: SDR sRGB), and what a surface without
/// a description is taken to be.
inline constexpr color_description k_srgb_description{};

/// The default luminances a named transfer function implies (protocol): PQ 0.005 / 10 000 /
/// 203, everything else sRGB's 0.2 / 80 / 80.
constexpr void set_default_luminances(color_description& d) {
    if (d.tf == color_tf::st2084_pq) {
        d.min_lum = 0.005f;
        d.max_lum = 10000.f;
        d.ref_lum = 203.f;
    } else {
        d.min_lum = 0.2f;
        d.max_lum = 80.f;
        d.ref_lum = 80.f;
    }
}

/// Knee of the tone curve, in units of the output's white: below it nothing changes.
inline constexpr float k_tone_knee = 0.8f;

/// A draw's colour transform into the working space (see above). `active` false: the
/// draw is an sRGB source and every backend draws it on its ordinary path.
struct color_transform {
    bool active = false;
    color_tf tf = color_tf::srgb;
    float power = 2.2f;
    /// Row-major, the luminance anchor included: working = matrix · E(V).
    float matrix[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    /// The brightest working value the content holds; > 1 tone maps.
    float peak = 1.f;

    [[nodiscard]] friend constexpr bool operator==(const color_transform&,
                                                   const color_transform&) = default;
};

/// The source primaries → BT.709 matrix (linear light, Bradford-adapted to D65), row-major,
/// in double. False when the primaries are degenerate (collinear, a white of y ≤ 0, not
/// finite): no matrix exists.
[[nodiscard]] bool primaries_to_bt709(const chromaticities& src, double out[9]);

/// The transform for `d` (see "Colour management" above). False for degenerate primaries
/// or luminances (ref ≤ min, max ≤ min, not finite): the description cannot be drawn.
[[nodiscard]] bool make_color_transform(const color_description& d, color_transform& out);

/// The tone curve on the largest channel `x` of a working-space colour whose content peaks
/// at `peak` (> 1): what to multiply all three channels by (ratios kept). Identity up to the
/// knee k = `k_tone_knee`; above it the excess t = (x − k) / (1 − k) is compressed by an
/// extended Reinhard f(t) = t · (1 + t / w²) / (1 + t) with w = (peak − k) / (1 − k), so
/// y = k + (1 − k) · f(t): continuous with slope 1 at the knee, monotonic, and `peak`
/// lands exactly on 1.0 — the output's white. Nothing below `peak` clips.
[[nodiscard]] inline float tone_map_scale(float x, float peak) {
    if (!(x > k_tone_knee) || !(peak > 1.f))
        return 1.f;
    const float w = (peak - k_tone_knee) / (1.f - k_tone_knee);
    const float t = (x - k_tone_knee) / (1.f - k_tone_knee);
    const float y = k_tone_knee + (1.f - k_tone_knee) * (t * (1.f + t / (w * w)) / (1.f + t));
    return y / x;
}

/// E(V) of one channel — the CPU path's decode, the reference the shaders follow.
[[nodiscard]] inline float decode_channel(float v, color_tf tf, float power) {
    switch (tf) {
    case color_tf::ext_linear: return v;
    case color_tf::power: return v < 0.f ? -std::pow(-v, power) : std::pow(v, power);
    case color_tf::st2084_pq: return lx::pq_to_linear(v < 0.f ? 0.f : v);
    case color_tf::srgb:
    default: return lx::srgb_to_linear(v);
    }
}

/// One straight (unpremultiplied) encoded colour through `t`: working-space linear light,
/// tone mapped. The CPU path's per-pixel transform.
inline void apply_color_transform(const color_transform& t, const float in[3], float out[3]) {
    const float e[3] = {decode_channel(in[0], t.tf, t.power), decode_channel(in[1], t.tf, t.power),
                        decode_channel(in[2], t.tf, t.power)};
    for (unsigned r = 0; r < 3; ++r)
        out[r] = t.matrix[r * 3] * e[0] + t.matrix[r * 3 + 1] * e[1] + t.matrix[r * 3 + 2] * e[2];
    if (t.peak > 1.f) {
        const float x = std::max(out[0], std::max(out[1], out[2]));
        const float k = tone_map_scale(x, t.peak);
        out[0] *= k;
        out[1] *= k;
        out[2] *= k;
    }
}

} // namespace lx::gfx

namespace {

/// 3 × 3 inverse (row-major); false when singular.
bool invert3(const double m[9], double out[9]) {
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(det) || std::fabs(det) < 1e-12)
        return false;
    const double inv = 1.0 / det;
    out[0] = c00 * inv;
    out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
    out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
    out[3] = c01 * inv;
    out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
    out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
    out[6] = c02 * inv;
    out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
    out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
    return true;
}

void mul3(const double a[9], const double b[9], double out[9]) {
    double r[9];
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    for (unsigned i = 0; i < 9; ++i)
        out[i] = r[i];
}

/// RGB → XYZ for `c` (white at Y = 1). Built from the chromaticity columns (x, y, 1 − x − y)
/// scaled to sum to the white, so a primary with y = 0 (CIE XYZ's own) needs no division.
bool rgb_to_xyz(const lx::gfx::chromaticities& c, double out[9]) {
    const double p[9] = {c.rx, c.gx, c.bx, c.ry, c.gy, c.by,
                         1.0 - c.rx - c.ry, 1.0 - c.gx - c.gy, 1.0 - c.bx - c.by};
    for (const double v : p)
        if (!std::isfinite(v))
            return false;
    if (!(c.wy > 0.f) || !std::isfinite(c.wx))
        return false;
    const double w[3] = {static_cast<double>(c.wx) / c.wy, 1.0,
                         (1.0 - c.wx - c.wy) / static_cast<double>(c.wy)};
    double pinv[9];
    if (!invert3(p, pinv))
        return false;
    double s[3];
    for (unsigned i = 0; i < 3; ++i)
        s[i] = pinv[i * 3] * w[0] + pinv[i * 3 + 1] * w[1] + pinv[i * 3 + 2] * w[2];
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            out[i * 3 + j] = p[i * 3 + j] * s[j];
    return true;
}

/// Bradford chromatic adaptation from white `src` to white `dst` (xy), in XYZ.
void bradford(double sx, double sy, double dx, double dy, double out[9]) {
    static constexpr double k_b[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135,
                                      0.0367, 0.0389, -0.0685, 1.0296};
    double binv[9];
    (void)invert3(k_b, binv);
    const double ws[3] = {sx / sy, 1.0, (1.0 - sx - sy) / sy};
    const double wd[3] = {dx / dy, 1.0, (1.0 - dx - dy) / dy};
    double cs[3], cd[3];
    for (unsigned i = 0; i < 3; ++i) {
        cs[i] = k_b[i * 3] * ws[0] + k_b[i * 3 + 1] * ws[1] + k_b[i * 3 + 2] * ws[2];
        cd[i] = k_b[i * 3] * wd[0] + k_b[i * 3 + 1] * wd[1] + k_b[i * 3 + 2] * wd[2];
    }
    const double d[9] = {cd[0] / cs[0], 0.0, 0.0, 0.0, cd[1] / cs[1], 0.0, 0.0, 0.0, cd[2] / cs[2]};
    double t[9];
    mul3(d, k_b, t);
    mul3(binv, t, out);
}

} // namespace

bool lx::gfx::primaries_to_bt709(const chromaticities& src, double out[9]) {
    const chromaticities bt709{};
    double to_xyz[9], dst_to_xyz[9], from_xyz[9];
    if (!rgb_to_xyz(src, to_xyz) || !rgb_to_xyz(bt709, dst_to_xyz) ||
        !invert3(dst_to_xyz, from_xyz))
        return false;
    double m[9];
    if (src.wx == bt709.wx && src.wy == bt709.wy) {
        mul3(from_xyz, to_xyz, m);
    } else {
        double adapt[9], t[9];
        bradford(src.wx, src.wy, bt709.wx, bt709.wy, adapt);
        mul3(adapt, to_xyz, t);
        mul3(from_xyz, t, m);
    }
    for (unsigned i = 0; i < 9; ++i) {
        if (!std::isfinite(m[i]))
            return false;
        out[i] = m[i];
    }
    return true;
}

bool lx::gfx::make_color_transform(const color_description& d, color_transform& out) {
    out = {};
    const double min = d.min_lum;
    const double ref = d.ref_lum;
    const double max = d.tf == color_tf::st2084_pq ? min + 10000.0 : static_cast<double>(d.max_lum);
    if (!std::isfinite(min) || !std::isfinite(ref) || !std::isfinite(max) || !(ref > min) ||
        !(max > min) || min < 0.0)
        return false;
    double m[9];
    if (!primaries_to_bt709(d.primaries, m))
        return false;
    // Signal 1.0 is `max` (10 000 above `min` for PQ), anchored at `ref`.
    const double scale = d.tf == color_tf::st2084_pq ? 10000.0 / (ref - min) : (max - min) / (ref - min);
    for (unsigned i = 0; i < 9; ++i)
        out.matrix[i] = static_cast<float>(m[i] * scale);
    double peak_nits = max;
    if (d.max_cll > 0.f)
        peak_nits = d.max_cll;
    else if (d.target_max_lum > 0.f)
        peak_nits = d.target_max_lum;
    double peak = (peak_nits - min) / (ref - min);
    // A bounded signal cannot exceed its own maximum.
    if (d.tf != color_tf::ext_linear && peak > scale)
        peak = scale;
    out.peak = peak > 1.0 ? static_cast<float>(peak) : 1.f;
    out.tf = d.tf;
    out.power = d.tf == color_tf::power ? d.power : 2.2f;
    bool identity = d.tf == color_tf::srgb && !(out.peak > 1.f);
    for (unsigned i = 0; i < 9 && identity; ++i)
        identity = std::fabs(m[i] * scale - ((i % 4) == 0 ? 1.0 : 0.0)) < 1e-6;
    if (identity) {
        out = {};
        return true;
    }
    out.active = true;
    return true;
}
