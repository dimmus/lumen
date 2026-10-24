#version 450
#extension GL_GOOGLE_include_directive : require
// lx.gfx:ui_vk, the quad pipeline's vert stage. The code is ui/quad.vs.glsl, the same file
// lx.gfx:ui_gl compiles for GLES; ui/vk_*.glsl is the Vulkan prologue.
#define LX_STRIDE 20u
#include "ui/vk_vertex.glsl"
#include "ui/quad.vs.glsl"
