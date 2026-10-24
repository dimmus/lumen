#version 450
#extension GL_GOOGLE_include_directive : require
// lx.gfx:ui_vk, the mono pipeline's frag stage. The code is ui/sprite.fs.glsl, the same file
// lx.gfx:ui_gl compiles for GLES; ui/vk_*.glsl is the Vulkan prologue.
#define LX_STRIDE 16u
#define LX_SPRITE 1
#include "ui/vk_fragment.glsl"
#include "ui/sprite.fs.glsl"
