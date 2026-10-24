// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// Deterministic helpers for both stages (`ui_math` / `ui_raster` of :ui_cpu).
const float LX_PI = 3.14159265358979323846;
float lx_saturate(float v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
float lx_sin(float x) {
    float turns = floor(x / (2.0 * LX_PI) + 0.5);
    x = x - turns * (2.0 * LX_PI);
    if (x > LX_PI * 0.5)
        x = LX_PI - x;
    else if (x < -LX_PI * 0.5)
        x = -LX_PI - x;
    float x2 = x * x;
    float term = x;
    float sum = x;
    for (int k = 2; k <= 12; k += 2) {
        term = -term * x2 / float(k * (k + 1));
        sum = sum + term;
    }
    return sum;
}
float lx_cos(float x) { return lx_sin(x + LX_PI * 0.5); }
float lx_srgb_decode(float v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
float lx_srgb_encode(float v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
float lx_cbrt(float x) { return x == 0.0 ? 0.0 : (x < 0.0 ? -pow(-x, 1.0 / 3.0) : pow(x, 1.0 / 3.0)); }
vec4 lx_to_oklab(vec4 c) {
    float inv = c.a > 0.0 ? 1.0 / c.a : 0.0;
    float r = lx_srgb_decode(lx_saturate(c.r * inv));
    float g = lx_srgb_decode(lx_saturate(c.g * inv));
    float b = lx_srgb_decode(lx_saturate(c.b * inv));
    float l = lx_cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    float m = lx_cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    float s = lx_cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return vec4(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s, c.a);
}
vec4 lx_from_oklab(vec4 lab) {
    float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;
    float r = lx_saturate(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
    float g = lx_saturate(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
    float b = lx_saturate(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
    float a = lx_saturate(lab.w);
    return vec4(lx_srgb_encode(r) * a, lx_srgb_encode(g) * a, lx_srgb_encode(b) * a, a);
}
// A rounded box as (centre x, centre y, half w, half h), half w < 0 when empty; radii
// top-left, top-right, bottom-right, bottom-left, clamped to [0, min(half size)].
vec4 lx_make_box(vec4 rect, vec4 radii, out vec4 r) {
    r = vec4(0.0);
    if (!(rect.z > 0.0) || !(rect.w > 0.0))
        return vec4(0.0, 0.0, -1.0, -1.0);
    float hw = rect.z * 0.5;
    float hh = rect.w * 0.5;
    float limit = min(hw, hh);
    r = min(max(radii, vec4(0.0)), vec4(limit));
    return vec4(rect.x + hw, rect.y + hh, hw, hh);
}
