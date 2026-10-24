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
} lx_pc;
uint inst_word(uint i) { return lx_words[lx_pc.base + uint(gl_InstanceIndex) * LX_STRIDE + i]; }
#define LX_VERTEX_ID gl_VertexIndex
#include "math.glsl"
#include "vertex.glsl"
