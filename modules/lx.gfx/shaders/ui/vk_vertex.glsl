// lx.gfx:ui_vk's vertex prologue (GLSL 4.50, Vulkan) — what `k_vertex_gl` in ui_gl.cppm is for
// GLES. Set 0: binding 0 the globals (the target size), binding 1 the frame's ring slot as
// words (std430, read only); the push constant holds the batch's first word in the slot and
// its atlas page. Every draw is vkCmdDraw(6, n, 0, 0): the batch's offset is the push
// constant, not firstInstance, so the instance word is `base + gl_InstanceIndex · stride`.
layout(std140, set = 0, binding = 0) uniform lx_globals {
    vec2 u_target;
    vec2 lx_globals_pad;
};
layout(std430, set = 0, binding = 1) readonly buffer lx_instances { uint lx_words[]; };
layout(push_constant) uniform lx_push {
    uint base;
    uint page;
    uint linear;    // 1: the attachment holds linear light (the compositor's F16 pass)
    float contrast; // the mono sprite coverage exponent there (theme text.contrast)
} lx_pc;
uint inst_word(uint i) { return lx_words[lx_pc.base + uint(gl_InstanceIndex) * LX_STRIDE + i]; }
#define LX_VERTEX_ID gl_VertexIndex
#include "math.glsl"
// Inside the compositor's linear F16 pass (ADR-010 4.5, 4.11) instance colours, emitted
// premultiplied and sRGB-encoded, become premultiplied linear light; in an encoded target they
// stay as they are.
#define LX_COLOR_HOOKS 1
vec4 lx_instance_color(vec4 c) { return lx_pc.linear != 0u ? lx_premul_to_linear(c) : c; }
#include "vertex.glsl"
