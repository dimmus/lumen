#version 450

// Encode pass. The composite pass blends in linear light into a float attachment; this
// converts that result into the transfer function the display expects and writes it to the
// scanout buffer.
//
// Splitting the encode out is what makes correct blending and wide output possible at the
// same time. Blending has to happen on linear values, so the attachment it blends into must
// hold linear values — but a scanout buffer holds encoded ones. Folding the encode into the
// composite pass would mean blending against encoded destinations again, which is the bug
// this whole path exists to avoid. It is also why 8-bit sRGB could be done with an sRGB
// attachment and 10-bit cannot: no 10-bit format has an sRGB variant for the hardware to
// encode into, so the conversion has to be explicit.
//
// Curves must match lx::from_linear in lx.foundation/types.cppm.

layout(push_constant) uniform encode_push {
    int transfer;   // 0 linear, 1 sRGB, 2 gamma 2.2, 3 PQ, 4 HLG
    float pad0;
    float pad1;
    float pad2;
} push;

layout(set = 0, binding = 0) uniform sampler2D linear_source;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 linear_to_srgb(vec3 v) {
    return mix(v * 12.92, 1.055 * pow(max(v, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
               greaterThan(v, vec3(0.0031308)));
}

vec3 linear_to_pq(vec3 v) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    vec3 y = pow(max(v, vec3(0.0)), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

vec3 linear_to_hlg(vec3 v) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 lo = sqrt(3.0 * max(v, vec3(0.0)));
    vec3 hi = a * log(max(12.0 * v - b, vec3(1e-6))) + c;
    return mix(lo, hi, greaterThan(v, vec3(1.0 / 12.0)));
}

void main() {
    vec4 texel = texture(linear_source, v_uv);

    // The composite pass wrote premultiplied linear color. Transfer functions are defined
    // on unpremultiplied signals, so undo the premultiply, encode, and restore it —
    // encoding premultiplied values directly darkens anything partially transparent.
    vec3 lin = texel.a > 0.0 ? texel.rgb / texel.a : texel.rgb;

    vec3 encoded;
    if (push.transfer == 0) {
        encoded = lin;
    } else if (push.transfer == 2) {
        encoded = pow(max(lin, vec3(0.0)), vec3(1.0 / 2.2));
    } else if (push.transfer == 3) {
        encoded = linear_to_pq(lin);
    } else if (push.transfer == 4) {
        encoded = linear_to_hlg(lin);
    } else {
        encoded = linear_to_srgb(lin);
    }

    out_color = vec4(encoded * texel.a, texel.a);
}
