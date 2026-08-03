#version 450

layout(push_constant) uniform quad_push {
    vec4 dst;
    vec2 target;
    float opacity;
    float pad;
    vec4 src_uv;
    vec4 tint;
    vec4 src_bounds;  // u_lo, v_lo, u_hi, v_hi — half-texel inset sampling bounds
} push;

layout(set = 0, binding = 0) uniform sampler2D surface_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    // Clamp to the source rectangle. Linear filtering samples between texel centers, so
    // a magnified crop would otherwise read texels from outside its own source rect.
    vec2 uv = clamp(v_uv, push.src_bounds.xy, push.src_bounds.zw);
    vec4 texel = texture(surface_texture, uv);
    // Client buffers are premultiplied, so opacity scales all four channels. The tint's
    // alpha scales the same way; its color channels multiply the already-premultiplied
    // RGB, which is what alpha-modifier and per-surface dimming both want.
    out_color = texel * vec4(push.tint.rgb, 1.0) * (push.opacity * push.tint.a);
}
