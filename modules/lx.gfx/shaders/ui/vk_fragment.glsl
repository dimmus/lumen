// lx.gfx:ui_vk's fragment prologue (GLSL 4.50, Vulkan) — what `k_fragment_gl` and
// `k_sprite_fs_gl` in ui_gl.cppm are for GLES. Sprites read set 0 binding 2, the eight atlas
// page slots (k_atlas_pages used, the rest a 1×1 dummy), by the push constant's page.
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform lx_push {
    uint base;
    uint page;
    uint linear;    // 1: the attachment holds linear light (the compositor's F16 pass)
    float contrast; // the mono sprite coverage exponent there (theme text.contrast)
} lx_pc;
#ifdef LX_SPRITE
layout(set = 0, binding = 2) uniform sampler2D lx_pages[8];
// A switch over constant indices, not `lx_pages[lx_pc.page]`: a non-constant index needs
// shaderSampledImageArrayDynamicIndexing, which the device the compositor creates does not
// enable. The page is uniform over the draw, so every invocation takes the same case.
vec4 lx_page_texel(ivec2 p) {
    switch (lx_pc.page) {
    case 0u: return texelFetch(lx_pages[0], p, 0);
    case 1u: return texelFetch(lx_pages[1], p, 0);
    case 2u: return texelFetch(lx_pages[2], p, 0);
    case 3u: return texelFetch(lx_pages[3], p, 0);
    case 4u: return texelFetch(lx_pages[4], p, 0);
    case 5u: return texelFetch(lx_pages[5], p, 0);
    case 6u: return texelFetch(lx_pages[6], p, 0);
    default: return texelFetch(lx_pages[7], p, 0);
    }
}
#endif
#include "math.glsl"
// Linear light (the compositor's F16 pass): colours made here are decoded like the instance
// colours, and a glyph's coverage goes through the text.contrast exponent so text blended in
// linear light keeps the weight it has blended in the encoded space (ADR-010 4.5).
#define LX_COLOR_HOOKS 1
vec4 lx_fragment_color(vec4 c) { return lx_pc.linear != 0u ? lx_premul_to_linear(c) : c; }
float lx_text_coverage(float c) { return lx_pc.linear != 0u && c > 0.0 ? pow(c, lx_pc.contrast) : c; }
#include "fragment.glsl"
