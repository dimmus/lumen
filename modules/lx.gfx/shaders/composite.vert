#version 450

// Surface quads are generated from the vertex index, so compositing a draw list needs
// no vertex or index buffers at all — only a push constant per quad.

layout(push_constant) uniform quad_push {
    vec4 dst;      // x, y, width, height in target pixels
    vec2 target;   // target size in pixels
    float opacity;
    float pad;
} push;

layout(location = 0) out vec2 v_uv;

const vec2 k_corners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    vec2 corner = k_corners[gl_VertexIndex];
    v_uv = corner;

    vec2 pixel = push.dst.xy + corner * push.dst.zw;
    vec2 ndc = (pixel / push.target) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
