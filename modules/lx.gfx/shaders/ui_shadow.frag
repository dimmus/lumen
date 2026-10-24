#version 450
#extension GL_GOOGLE_include_directive : require
// lx.gfx:ui_vk, the shadow pipeline's frag stage. The code is ui/shadow.fs.glsl, the same file
// lx.gfx:ui_gl compiles for GLES; ui/vk_*.glsl is the Vulkan prologue.
#define LX_STRIDE 16u
#include "ui/vk_fragment.glsl"
#include "ui/shadow.fs.glsl"
