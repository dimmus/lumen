#version 450
#extension GL_GOOGLE_include_directive : require
// lx.gfx:ui_vk, the shadow pipeline's vert stage. The code is ui/shadow.vs.glsl, the same file
// lx.gfx:ui_gl compiles for GLES; ui/vk_*.glsl is the Vulkan prologue.
#define LX_STRIDE 16u
#include "ui/vk_vertex.glsl"
#include "ui/shadow.vs.glsl"
