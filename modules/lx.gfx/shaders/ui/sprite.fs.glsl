// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// mono / poly sprite (LX_POLY), fragment. The prologue defines `vec4 lx_page_texel(ivec2)`,
// the texel of the batch's atlas page.
flat in vec4 v_bounds;
flat in vec4 v_tile;
flat in vec4 v_color;
flat in vec4 v_mbox;
flat in vec4 v_mr;
flat in uint v_flags;

void main() {
    vec2 p = gl_FragCoord.xy;
    vec4 texel;
    if ((v_flags & 1u) != 0u) {
        texel = lx_page_texel(ivec2(v_tile.xy) + (ivec2(floor(p)) - ivec2(v_bounds.xy)));
    } else {
        // `ui_raster::sample`: bilinear at (u - 1/2, v - 1/2), clamped to the tile's texel centres.
        float ku = (v_tile.z - v_tile.x) / v_bounds.z;
        float kv = (v_tile.w - v_tile.y) / v_bounds.w;
        float su = clamp(v_tile.x + (p.x - v_bounds.x) * ku - 0.5, v_tile.x, v_tile.z - 1.0);
        float sv = clamp(v_tile.y + (p.y - v_bounds.y) * kv - 0.5, v_tile.y, v_tile.w - 1.0);
        float fu = floor(su);
        float fv = floor(sv);
        float wx = su - fu;
        float wy = sv - fv;
        int i0 = int(fu);
        int j0 = int(fv);
        int i1 = i0 + 1 < int(v_tile.z) ? i0 + 1 : i0;
        int j1 = j0 + 1 < int(v_tile.w) ? j0 + 1 : j0;
        vec4 p00 = lx_page_texel(ivec2(i0, j0));
        vec4 p10 = lx_page_texel(ivec2(i1, j0));
        vec4 p01 = lx_page_texel(ivec2(i0, j1));
        vec4 p11 = lx_page_texel(ivec2(i1, j1));
        vec4 top = p00 + (p10 - p00) * wx;
        vec4 bottom = p01 + (p11 - p01) * wx;
        texel = top + (bottom - top) * wy;
    }
#ifdef LX_POLY
    vec4 c = texel;
    if ((v_flags & 2u) != 0u) {
        float l = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        c.rgb = vec3(l);
    }
    vec4 src = c * v_color;
    if ((v_flags & 4u) != 0u)
        src = src * lx_coverage(v_mbox, v_mr, p);
    o_color = src;
#else
    o_color = v_color * texel.r;
#endif
}
