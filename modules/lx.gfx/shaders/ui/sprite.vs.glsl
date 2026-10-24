// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// mono / poly sprite (LX_POLY), vertex
flat out vec4 v_bounds;
flat out vec4 v_tile;  // u0, v0, u1, v1 (texels)
flat out vec4 v_color;
flat out vec4 v_mbox;  // poly: the rounded mask
flat out vec4 v_mr;
flat out uint v_flags; // 1 nearest, 2 grayscale, 4 rounded; bits 3-5 the output transform
void main() {
    vec4 bounds = inst_vec4(0u);
    if (!lx_place(lx_window(bounds, inst_vec4(4u))))
        return;
    uint a = inst_word(8u);
    uint b = inst_word(9u);
    vec4 tile = vec4(float(a & 0xFFFFu), float(a >> 16u), float(b & 0xFFFFu), float(b >> 16u));
    // An output transform the view turned the sprite by (flags bits 1-3): `bounds` is the
    // turned box; the unturned one is what matches the tile.
    uint xf = (inst_word(10u) >> 9u) & 7u;
    vec2 o;
    vec2 bdx;
    vec2 bdy;
    lx_basis(xf, o, bdx, bdy);
    vec2 csize = bdx.x == 0.0 ? bounds.wz : bounds.zw;
    uint flags = xf << 3u;
    if (bounds.x == floor(bounds.x) && bounds.y == floor(bounds.y) && csize.x == tile.z - tile.x &&
        csize.y == tile.w - tile.y)
        flags |= 1u;
    vec4 mr = vec4(0.0);
    vec4 mbox = vec4(0.0, 0.0, -1.0, -1.0);
#ifdef LX_POLY
    if (((inst_word(10u) >> 8u) & 1u) != 0u)
        flags |= 2u;
    vec4 radii = inst_half4(13u);
    if (radii.x > 0.0 || radii.y > 0.0 || radii.z > 0.0 || radii.w > 0.0) {
        flags |= 4u;
        mbox = lx_make_box(bounds, radii, mr);
    }
#endif
    v_bounds = bounds;
    v_tile = tile;
    v_color = lx_instance_color(inst_half4(11u));
    v_mbox = mbox;
    v_mr = mr;
    v_flags = flags;
}
