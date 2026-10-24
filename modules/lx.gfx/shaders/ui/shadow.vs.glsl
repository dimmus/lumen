// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// shadow, vertex
flat out vec4 v_box;
flat out vec4 v_r;
flat out vec4 v_elem;
flat out vec4 v_er;
flat out vec4 v_color;
flat out vec4 v_sigma; // sigma, sqrt(1/2) / sigma, 1 / (sqrt(2 pi) sigma), -1 / (2 sigma^2)
flat out uint v_flags; // 1 inset, 2 hard
void main() {
    vec4 bounds = inst_vec4(0u);
    vec4 radii = inst_half4(8u);
    float sigma = max(inst_float(12u), 0.0);
    float sp = inst_float(13u);
    bool inset = (inst_word(14u) & 1u) != 0u;
    float reach = inset ? 0.0 : 3.0 * sigma + sp;
    vec4 raster = vec4(bounds.x - reach, bounds.y - reach, bounds.z + 2.0 * reach, bounds.w + 2.0 * reach);
    if (!lx_place(lx_window(raster, inst_vec4(4u))))
        return;
    uint flags = inset ? 1u : 0u;
    bool hard = sigma < 1.0e-4;
    if (hard)
        flags |= 2u;
    vec4 r;
    vec4 er = vec4(0.0);
    vec4 box;
    vec4 elem = vec4(0.0, 0.0, -1.0, -1.0);
    if (inset) {
        elem = lx_make_box(bounds, radii, er);
        box = lx_make_box(vec4(bounds.x + sp, bounds.y + sp, bounds.z - 2.0 * sp, bounds.w - 2.0 * sp),
                          max(radii - sp, vec4(0.0)), r);
    } else {
        box = lx_make_box(vec4(bounds.x - sp, bounds.y - sp, bounds.z + 2.0 * sp, bounds.w + 2.0 * sp),
                          max(radii + sp, vec4(0.0)), r);
    }
    v_sigma = hard ? vec4(sigma, 0.0, 0.0, 0.0)
                   : vec4(sigma, 0.70710678118 / sigma, 1.0 / (2.50662827463 * sigma),
                          -1.0 / (2.0 * sigma * sigma));
    v_box = box;
    v_r = r;
    v_elem = elem;
    v_er = er;
    v_color = inst_half4(10u);
    v_flags = flags;
}
