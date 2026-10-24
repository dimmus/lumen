// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// The fragment stage's rounded-box coverage. The prologue declares `o_color`.
// Colours made in the fragment stage (an Oklab gradient, a poly sprite's texel) and a mono
// sprite's coverage, as the target blends them: unchanged in the encoded space; a prologue
// that blends in linear light defines LX_COLOR_HOOKS and its own versions before this file.
#ifndef LX_COLOR_HOOKS
vec4 lx_fragment_color(vec4 c) { return c; }
float lx_text_coverage(float c) { return c; }
#endif
float lx_pick(vec4 r, vec2 d) { return d.x < 0.0 ? (d.y < 0.0 ? r.x : r.w) : (d.y < 0.0 ? r.y : r.z); }
// GPUI's rounded-box signed distance (negative inside), as `ui_raster::sdf`.
float lx_sdf(vec4 box, vec4 r, vec2 p) {
    vec2 d = p - box.xy;
    float rr = lx_pick(r, d);
    float qx = abs(d.x) - box.z + rr;
    float qy = abs(d.y) - box.w + rr;
    if (rr == 0.0)
        return max(qx, qy);
    float mx = max(qx, 0.0);
    float my = max(qy, 0.0);
    float len = mx == 0.0 ? my : (my == 0.0 ? mx : sqrt(mx * mx + my * my));
    return len + min(max(qx, qy), 0.0) - rr;
}
float lx_coverage(vec4 box, vec4 r, vec2 p) {
    return box.z < 0.0 ? 0.0 : lx_saturate(0.5 - lx_sdf(box, r, p));
}
