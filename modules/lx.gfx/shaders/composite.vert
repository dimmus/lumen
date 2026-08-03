#version 450

// Surface quads are generated from the vertex index, so compositing a draw list needs
// no vertex or index buffers at all — only a push constant per quad.

layout(push_constant) uniform quad_push {
    vec4 dst;      // x, y, width, height in target pixels
    vec2 target;   // target size in pixels
    float opacity;
    float pad;
    vec4 src_uv;   // u0, v0, du, dv — the source rectangle, normalized
    vec4 tint;
    vec4 src_bounds;
    int transfer;
    float pad1;
    float pad2;
    float pad3;
} push;

layout(location = 0) out vec2 v_uv;

const vec2 k_corners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    vec2 corner = k_corners[gl_VertexIndex];
    // Sample the source sub-rectangle rather than the whole texture, so a cropped or
    // fractionally scaled surface is the same draw with different UVs.
    v_uv = push.src_uv.xy + corner * push.src_uv.zw;

    vec2 pixel = push.dst.xy + corner * push.dst.zw;
    vec2 ndc = (pixel / push.target) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
