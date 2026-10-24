// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// The vertex stage: instance unpacking, the raster window and the whole-pixel quad. The
// prologue defines `uint inst_word(uint)`, `u_target` (the target size) and LX_VERTEX_ID.
float inst_float(uint i) { return uintBitsToFloat(inst_word(i)); }
vec4 inst_vec4(uint i) {
    return vec4(inst_float(i), inst_float(i + 1u), inst_float(i + 2u), inst_float(i + 3u));
}
vec4 inst_half4(uint i) { return vec4(unpackHalf2x16(inst_word(i)), unpackHalf2x16(inst_word(i + 1u))); }

float lx_clamp_px(float v) {
    if (!(v >= -1.0e6))
        return -1.0e6;
    if (!(v <= 1.0e6))
        return 1.0e6;
    return v;
}
// The pixels whose centre lies in [lo, hi): [ceil(lo - 1/2), ceil(hi - 1/2)).
vec2 lx_centres(float lo, float hi) {
    return vec2(lx_clamp_px(ceil(lo - 0.5)), lx_clamp_px(ceil(hi - 0.5)));
}
// x0, y0, x1, y1 of the pixels whose centre is in `r` and in `clip` (no mask when empty),
// within the target.
vec4 lx_window(vec4 r, vec4 clip) {
    vec2 xs = lx_centres(r.x, r.x + r.z);
    vec2 ys = lx_centres(r.y, r.y + r.w);
    vec4 w = vec4(xs.x, ys.x, xs.y, ys.y);
    if (clip.z > 0.0 && clip.w > 0.0) {
        vec2 cx = lx_centres(clip.x, clip.x + clip.z);
        vec2 cy = lx_centres(clip.y, clip.y + clip.w);
        w = vec4(max(w.x, cx.x), max(w.y, cy.x), min(w.z, cx.y), min(w.w, cy.y));
    }
    return vec4(max(w.xy, vec2(0.0)), min(w.zw, u_target));
}
// The instance colour as the stage hands it to the fragment: the premultiplied encoded colour
// as emitted. A prologue that blends in linear light (ui_vk inside the compositor's F16 pass)
// defines LX_COLOR_HOOKS and its own `lx_instance_color` before this file.
#ifndef LX_COLOR_HOOKS
vec4 lx_instance_color(vec4 c) { return c; }
#endif
void lx_cull() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); }
// Places this vertex of the window's two triangles; false (and culled) when it is empty.
bool lx_place(vec4 w) {
    if (!(w.z > w.x) || !(w.w > w.y)) {
        lx_cull();
        return false;
    }
    int v = LX_VERTEX_ID;
    vec2 corner = vec2((v == 1 || v == 4 || v == 5) ? 1.0 : 0.0, (v == 2 || v == 3 || v == 5) ? 1.0 : 0.0);
    vec2 p = mix(w.xy, w.zw, corner);
    gl_Position = vec4(p / u_target * 2.0 - 1.0, 0.0, 1.0);
    return true;
}
