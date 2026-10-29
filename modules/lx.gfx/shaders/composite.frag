#version 450
#extension GL_GOOGLE_include_directive : require

// Composite pass. Writes premultiplied **linear light** into a float attachment, so the
// fixed-function blender averages light rather than encoded values — alpha blending is a
// weighted average of light, and averaging gamma-encoded values darkens edges (the classic
// dark fringe around antialiased text).
//
// The encode back into the display's transfer function happens in encode.frag, against the
// blended result. Curves must match lx::to_linear in lx.foundation/types.cppm.

layout(push_constant) uniform quad_push {
    vec4 dst;
    vec2 target;
    float opacity;
    float use_solid; // 1: a solid draw, `solid` replaces the sample
    vec4 src_uv;
    vec4 tint;
    vec4 src_bounds;  // u_lo, v_lo, u_hi, v_hi — half-texel inset sampling bounds
    int transfer;     // 0 linear, 1 sRGB, 2 gamma 2.2, 3 PQ, 4 HLG
    int downsample;   // 1: the area filter (downsample.glsl)
    vec2 uv_dy;
    vec4 solid;      // premultiplied, encoded colour of a solid draw
} push;

layout(set = 0, binding = 0) uniform sampler2D surface_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

#include "downsample.glsl"

vec3 srgb_to_linear(vec3 v) {
    return mix(v / 12.92, pow((max(v, vec3(0.0)) + 0.055) / 1.055, vec3(2.4)),
               greaterThan(v, vec3(0.04045)));
}

vec3 pq_to_linear(vec3 v) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    vec3 e = pow(max(v, vec3(0.0)), vec3(1.0 / m2));
    vec3 num = max(e - c1, vec3(0.0));
    vec3 den = max(c2 - c3 * e, vec3(1e-6));
    return pow(num / den, vec3(1.0 / m1));
}

vec3 hlg_to_linear(vec3 v) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 lo = (v * v) / 3.0;
    vec3 hi = (exp((v - c) / a) + b) / 12.0;
    return mix(lo, hi, greaterThan(v, vec3(0.5)));
}

vec3 decode(vec3 v) {
    if (push.transfer == 0) return v;
    if (push.transfer == 2) return pow(max(v, vec3(0.0)), vec3(2.2));
    if (push.transfer == 3) return pq_to_linear(v);
    if (push.transfer == 4) return hlg_to_linear(v);
    return srgb_to_linear(v);
}

void main() {
    // Clamped to the source rectangle (in sample_source). Linear filtering samples between
    // texel centers, so a magnified crop would otherwise read texels from outside its own
    // source rect. A solid draw (wp_single_pixel_buffer_v1) takes the colour a 1x1 texture
    // would hold and goes through the same decode, tint and opacity.
    vec4 texel = push.use_solid > 0.5
                     ? push.solid
                     : sample_source(surface_texture, v_uv, push.src_bounds,
                                     push.src_uv.zw / push.dst.z, push.uv_dy / push.dst.w,
                                     push.downsample);

    // Client buffers are premultiplied, but a transfer function is defined on the
    // unpremultiplied signal — decoding premultiplied values would bend partially
    // transparent pixels toward black.
    vec3 straight = texel.a > 0.0 ? texel.rgb / texel.a : texel.rgb;
    vec3 lin = decode(straight);

    // Tint and opacity scale light, so they apply after decoding. The tint's color
    // channels are treated as linear multipliers.
    float alpha = texel.a * push.opacity * push.tint.a;
    out_color = vec4(lin * push.tint.rgb * alpha, alpha);
}
