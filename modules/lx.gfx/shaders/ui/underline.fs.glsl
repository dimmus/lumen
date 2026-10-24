// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// underline, fragment
flat in vec4 v_bounds;
flat in vec4 v_color;
flat in vec4 v_line;
flat in uint v_wavy;
void main() {
    vec2 p = gl_FragCoord.xy;
    float dist;
    if (v_wavy != 0u) {
        float phase = (p.x - v_bounds.x) * v_line.z;
        float fy = v_line.w + v_line.x * lx_sin(phase);
        float slope = v_line.x * v_line.z * lx_cos(phase);
        dist = abs(p.y - fy) / sqrt(1.0 + slope * slope);
    } else {
        dist = abs(p.y - v_line.w);
    }
    float cov = lx_saturate(v_line.y - dist + 0.5);
    o_color = cov > 0.0 ? v_color * cov : vec4(0.0);
}
