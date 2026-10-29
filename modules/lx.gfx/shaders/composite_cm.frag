#version 450
#extension GL_GOOGLE_include_directive : require

// Composite pass, colour-managed draws (wp_color_manager_v1): composite.frag plus the draw's
// colour transform into the working space — linear light, BT.709 primaries, 1.0 = the
// output's reference white ("Colour management" in lx.gfx color.cppm). A draw whose
// transform is inactive (every sRGB surface) never uses this shader.
//
//   E  decode: 0 ext_linear (unbounded), 1 sRGB, 2 power curve (exponent m0.w, mirrored
//      through 0), 3 PQ (1.0 = 10 000 cd/m²; the luminance anchor is in the matrix)
//   M  rows m0.xyz, m1.xyz, m2.xyz: primaries to BT.709 and the anchor, in linear light
//   T  tone map when the content peaks above the output's white (m1.w > 1):
//      lx::gfx::tone_map_scale on the largest channel, all three scaled alike
//
// m2.w = 1: the attachment holds encoded values (a UI target, `record_blits` encoded): the
// result is clamped and sRGB-encoded here, as gl_compositor does.

layout(push_constant) uniform quad_push {
    vec4 dst;
    vec2 target;
    float opacity;
    float use_solid;
    vec4 src_uv;
    vec4 tint;
    vec4 src_bounds;
    int transfer;
    int downsample;
    vec2 uv_dy;
    vec4 solid;
    vec4 m0;
    vec4 m1;
    vec4 m2;
} push;

layout(set = 0, binding = 0) uniform sampler2D surface_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

#include "downsample.glsl"

const float k_knee = 0.8; // lx::gfx::k_tone_knee

vec3 srgb_to_linear(vec3 v) {
    return mix(v / 12.92, pow((max(v, vec3(0.0)) + 0.055) / 1.055, vec3(2.4)),
               greaterThan(v, vec3(0.04045)));
}

vec3 linear_to_srgb(vec3 v) {
    return mix(v * 12.92, 1.055 * pow(max(v, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
               greaterThan(v, vec3(0.0031308)));
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

vec3 decode(vec3 v) {
    if (push.transfer == 0) return v;
    if (push.transfer == 2) return sign(v) * pow(abs(v), vec3(push.m0.w));
    if (push.transfer == 3) return pq_to_linear(v);
    return srgb_to_linear(v);
}

void main() {
    vec4 texel = push.use_solid > 0.5
                     ? push.solid
                     : sample_source(surface_texture, v_uv, push.src_bounds,
                                     push.src_uv.zw / push.dst.z, push.uv_dy / push.dst.w,
                                     push.downsample);

    // Transfer functions are defined on the straight signal, as in composite.frag.
    vec3 straight = texel.a > 0.0 ? texel.rgb / texel.a : texel.rgb;
    vec3 e = decode(straight);
    vec3 lin = vec3(dot(push.m0.xyz, e), dot(push.m1.xyz, e), dot(push.m2.xyz, e));

    float peak = push.m1.w;
    if (peak > 1.0) {
        float x = max(lin.r, max(lin.g, lin.b));
        if (x > k_knee) {
            float w = (peak - k_knee) / (1.0 - k_knee);
            float t = (x - k_knee) / (1.0 - k_knee);
            float y = k_knee + (1.0 - k_knee) * (t * (1.0 + t / (w * w)) / (1.0 + t));
            lin *= y / x;
        }
    }

    float alpha = texel.a * push.opacity * push.tint.a;
    vec3 c = lin * push.tint.rgb;
    if (push.m2.w > 0.5)
        c = linear_to_srgb(clamp(c, 0.0, 1.0));
    out_color = vec4(c * alpha, alpha);
}
