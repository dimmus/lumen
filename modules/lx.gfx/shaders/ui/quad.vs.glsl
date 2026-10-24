// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// quad, vertex
flat out vec4 v_bounds;
flat out vec4 v_r;
flat out vec4 v_ibox;
flat out vec4 v_ir;
flat out vec4 v_widths;
flat out vec4 v_bg0;
flat out vec4 v_bg1;
flat out vec4 v_border;
flat out vec4 v_grad;
flat out vec4 v_lab0;
flat out vec4 v_lab1;
flat out uint v_flags;
void main() {
    vec4 bounds = inst_vec4(0u);
    vec4 r;
    vec4 box = lx_make_box(bounds, inst_half4(8u), r);
    if (box.z < 0.0) {
        lx_cull();
        return;
    }
    if (!lx_place(lx_window(bounds, inst_vec4(4u))))
        return;
    vec4 widths = max(inst_half4(10u), vec4(0.0)); // top, right, bottom, left
    uint flags = 0u;
    vec4 ir = vec4(0.0);
    vec4 ibox = vec4(0.0, 0.0, -1.0, -1.0);
    bool border = widths.x > 0.0 || widths.y > 0.0 || widths.z > 0.0 || widths.w > 0.0;
    if (border) {
        flags |= 1u;
        vec4 inner_radii = vec4(max(r.x - max(widths.x, widths.w), 0.0),
                                max(r.y - max(widths.x, widths.y), 0.0),
                                max(r.z - max(widths.z, widths.y), 0.0),
                                max(r.w - max(widths.z, widths.w), 0.0));
        ibox = lx_make_box(vec4(bounds.x + widths.w, bounds.y + widths.x,
                                bounds.z - widths.w - widths.y, bounds.w - widths.x - widths.z),
                           inner_radii, ir);
    }
    uint style = inst_word(18u); // angle f16 | border_style << 16 | gradient_space << 24
    if (border && ((style >> 16u) & 0xFFu) == 1u)
        flags |= 2u;
    v_bg0 = inst_half4(12u);
    v_bg1 = inst_half4(14u);
    v_border = inst_half4(16u);
    v_grad = vec4(1.0, 0.0, 0.0, 0.0);
    v_lab0 = vec4(0.0);
    v_lab1 = vec4(0.0);
    if (inst_word(12u) != inst_word(14u) || inst_word(13u) != inst_word(15u)) {
        flags |= 4u;
        float theta = unpackHalf2x16(style & 0xFFFFu).x * (LX_PI / 180.0);
        float dx = lx_cos(theta);
        float dy = lx_sin(theta);
        float len = abs(bounds.z * dx) + abs(bounds.w * dy);
        v_grad = vec4(dx, dy, len > 0.0 ? 1.0 / len : 0.0, 0.0);
        if (((style >> 24u) & 0xFFu) == 1u) {
            flags |= 8u;
            v_lab0 = lx_to_oklab(v_bg0);
            v_lab1 = lx_to_oklab(v_bg1);
        }
    }
    v_bounds = bounds;
    v_r = r;
    v_ibox = ibox;
    v_ir = ir;
    v_widths = widths;
    v_flags = flags;
}
