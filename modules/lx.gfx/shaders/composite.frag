#version 450

layout(push_constant) uniform quad_push {
    vec4 dst;
    vec2 target;
    float opacity;
    float pad;
} push;

layout(set = 0, binding = 0) uniform sampler2D surface_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    // Client buffers are premultiplied, so opacity scales all four channels.
    out_color = texture(surface_texture, v_uv) * push.opacity;
}
