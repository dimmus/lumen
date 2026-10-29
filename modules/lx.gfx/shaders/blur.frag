#version 450

// Backdrop blur (ext-background-effect-v1; lx.gfx:renderer "Backdrop blur"): one pass of the
// dual-Kawase chain into one level, drawn as encode.vert's fullscreen triangle over the
// level. gl_FragCoord is the destination texel's centre, row 0 at the top as in the image.
//
//   mode 0  level 0 from the linear intermediate: the footprint (origin `origin`) sampled at
//           the corner of each 2×2 block — the block's mean.
//   mode 1  down: 4·S(c) + the four diagonal taps at ±offset, / 8, c = 2·frag.
//   mode 2  up: the four axis taps at ±offset, twice the four diagonals at ±offset/2, / 12,
//           c = frag / 2.
//
// Every tap is clamped to the source's valid texel centres (`valid`), whatever the image
// holds past them; `inv_size` is one over its allocated size.

layout(push_constant) uniform blur_push {
    vec2 valid;
    vec2 inv_size;
    vec2 origin;
    float offset;
    int mode;
} push;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec4 tap(vec2 p) {
    return texture(source, clamp(p, vec2(0.5), push.valid - vec2(0.5)) * push.inv_size);
}

void main() {
    vec2 f = gl_FragCoord.xy;
    if (push.mode == 0) {
        out_color = tap(push.origin + f * 2.0);
    } else if (push.mode == 1) {
        vec2 c = f * 2.0;
        float o = push.offset;
        out_color = (4.0 * tap(c) + tap(c + vec2(-o, -o)) + tap(c + vec2(o, o)) +
                     tap(c + vec2(o, -o)) + tap(c + vec2(-o, o))) / 8.0;
    } else {
        vec2 c = f * 0.5;
        float h = push.offset * 0.5;
        vec4 sum = tap(c + vec2(-2.0 * h, 0.0)) + tap(c + vec2(2.0 * h, 0.0)) +
                   tap(c + vec2(0.0, -2.0 * h)) + tap(c + vec2(0.0, 2.0 * h));
        sum += 2.0 * (tap(c + vec2(-h, -h)) + tap(c + vec2(h, h)) + tap(c + vec2(h, -h)) +
                      tap(c + vec2(-h, h)));
        out_color = sum / 12.0;
    }
}
