// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// underline, vertex
flat out vec4 v_bounds;
flat out vec4 v_color;
flat out vec4 v_line; // thickness, thickness / 2, omega, centre y
flat out uint v_wavy;
void main() {
    vec4 bounds = inst_vec4(0u);
    uint tw = inst_word(10u); // thickness f16 | wavy << 16
    float t = max(unpackHalf2x16(tw & 0xFFFFu).x, 0.0);
    if (!(t > 0.0)) {
        lx_cull();
        return;
    }
    if (!lx_place(lx_window(bounds, inst_vec4(4u))))
        return;
    v_bounds = bounds;
    v_color = inst_half4(8u);
    v_line = vec4(t, t * 0.5, 2.0 * LX_PI / (4.0 * t), bounds.y + bounds.w * 0.5);
    v_wavy = tw >> 16u;
}
