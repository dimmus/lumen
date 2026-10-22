#version 450

// Surface quads are generated from the vertex index, so compositing a draw list needs
// no vertex or index buffers at all — only a push constant per quad.

layout(push_constant) uniform quad_push {
    vec4 dst;      // x, y, width, height in target pixels
    vec2 target;   // target size in pixels
    float opacity;
    float use_solid; // 1: a solid draw, `solid` replaces the sample
    vec4 src_uv;   // the source box as a basis: top-left (xy) and +x direction (zw)
    vec4 tint;
    vec4 src_bounds;
    int transfer;
    float pad1;
    vec2 uv_dy;    // +y direction of the source basis
    vec4 solid;      // premultiplied, encoded colour of a solid draw
} push;

layout(location = 0) out vec2 v_uv;

const vec2 k_corners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    vec2 corner = k_corners[gl_VertexIndex];
    // Sample the source box through its basis, so a cropped, fractionally scaled or
    // transformed surface is the same draw with different vectors.
    v_uv = push.src_uv.xy + corner.x * push.src_uv.zw + corner.y * push.uv_dy;

    vec2 pixel = push.dst.xy + corner * push.dst.zw;
    vec2 ndc = (pixel / push.target) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
