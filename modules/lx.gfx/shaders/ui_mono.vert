#version 450
#extension GL_GOOGLE_include_directive : require
// lx.gfx:ui_vk, the mono pipeline's vert stage. The code is ui/sprite.vs.glsl, the same file
// lx.gfx:ui_gl compiles for GLES; ui/vk_*.glsl is the Vulkan prologue.
#define LX_STRIDE 16u
#define LX_SPRITE 1
#include "ui/vk_vertex.glsl"
#include "ui/sprite.vs.glsl"
