// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// shadow, fragment
flat in vec4 v_box;
flat in vec4 v_r;
flat in vec4 v_elem;
flat in vec4 v_er;
flat in vec4 v_color;
flat in vec4 v_sigma;
flat in uint v_flags;

// Abramowitz-Stegun 7.1.27, as `ui_math::erf`.
float lx_erf(float x) {
    float s = x < 0.0 ? -1.0 : (x > 0.0 ? 1.0 : 0.0);
    float a = abs(x);
    float t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t = t * t;
    return s - s / (t * t);
}

// Evan Wallace's blurred rounded box, four samples along y.
float lx_shadow_mask(vec2 p) {
    if (v_box.z < 0.0)
        return 0.0;
    if ((v_flags & 2u) != 0u)
        return lx_coverage(v_box, v_r, p);
    vec2 d = p - v_box.xy;
    float corner = lx_pick(v_r, d);
    float three = 3.0 * v_sigma.x;
    float low = d.y - v_box.w;
    float high = d.y + v_box.w;
    float start = min(max(-three, low), high);
    float end = min(max(three, low), high);
    float stride = (end - start) * 0.25;
    float y = start + stride * 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; ++i) {
        float yy = d.y - y;
        float delta = min(v_box.w - corner - abs(yy), 0.0);
        float curved = v_box.z - corner + sqrt(max(0.0, corner * corner - delta * delta));
        float lo = 0.5 + 0.5 * lx_erf((d.x - curved) * v_sigma.y);
        float hi = 0.5 + 0.5 * lx_erf((d.x + curved) * v_sigma.y);
        float gauss = exp(y * y * v_sigma.w) * v_sigma.z;
        value = value + (hi - lo) * gauss * stride;
        y = y + stride;
    }
    return value;
}

void main() {
    vec2 p = gl_FragCoord.xy;
    float mask;
    if ((v_flags & 1u) == 0u) {
        mask = lx_shadow_mask(p);
    } else {
        float element = lx_coverage(v_elem, v_er, p);
        mask = element > 0.0 ? element * (1.0 - lx_shadow_mask(p)) : 0.0;
    }
    o_color = mask > 0.0 ? v_color * mask : vec4(0.0);
}
