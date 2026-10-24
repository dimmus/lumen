// ADR-010 UI primitives, shared by both GPU backends: lx.gfx:ui_gl compiles it at run time
// (GLSL ES 3.10, embedded as a string) and lx.gfx:ui_vk at build time (GLSL 4.50 -> SPIR-V,
// through an #include). Keep it to the common subset; whatever differs between the two lives
// in the backend's prologue (ui_gl.cppm, vk_vertex.glsl / vk_fragment.glsl).
// quad, fragment
flat in vec4 v_bounds;
flat in vec4 v_r;
flat in vec4 v_ibox;
flat in vec4 v_ir;
flat in vec4 v_widths;
flat in vec4 v_bg0;
flat in vec4 v_bg1;
flat in vec4 v_border;
flat in vec4 v_grad;
flat in vec4 v_lab0;
flat in vec4 v_lab1;
flat in uint v_flags;

vec4 lx_background(vec2 p, vec4 box) {
    if ((v_flags & 4u) == 0u)
        return v_bg0;
    float t = lx_saturate(0.5 + ((p.x - box.x) * v_grad.x + (p.y - box.y) * v_grad.y) * v_grad.z);
    if ((v_flags & 8u) == 0u)
        return v_bg0 + (v_bg1 + v_bg0 * -1.0) * t;
    return lx_from_oklab(v_lab0 + (v_lab1 - v_lab0) * t);
}

float lx_dash(vec2 p) {
    vec4 r = v_r;
    vec4 w = v_widths;
    float x0 = v_bounds.x;
    float y0 = v_bounds.y;
    float x1 = v_bounds.x + v_bounds.z;
    float y1 = v_bounds.y + v_bounds.w;
    float tl_x = max(r.x, w.w), tl_y = max(r.x, w.x);
    float tr_x = max(r.y, w.y), tr_y = max(r.y, w.x);
    float br_x = max(r.z, w.y), br_y = max(r.z, w.z);
    float bl_x = max(r.w, w.w), bl_y = max(r.w, w.z);
    if ((p.x < x0 + tl_x && p.y < y0 + tl_y) || (p.x >= x1 - tr_x && p.y < y0 + tr_y) ||
        (p.x >= x1 - br_x && p.y >= y1 - br_y) || (p.x < x0 + bl_x && p.y >= y1 - bl_y))
        return 1.0;
    float excess[4] = float[4]((p.y - y0) - w.x, (x1 - p.x) - w.y, (y1 - p.y) - w.z, (p.x - x0) - w.w);
    float widths[4] = float[4](w.x, w.y, w.z, w.w);
    int edge = -1;
    float best = 0.0;
    for (int e = 0; e < 4; ++e) {
        if (!(widths[e] > 0.0))
            continue;
        if (edge < 0 || excess[e] < best) {
            best = excess[e];
            edge = e;
        }
    }
    if (edge < 0)
        return 1.0;
    float t;
    if (edge == 0)
        t = p.x - (x0 + tl_x);
    else if (edge == 1)
        t = p.y - (y0 + tr_y);
    else if (edge == 2)
        t = (x1 - br_x) - p.x;
    else
        t = (y1 - bl_y) - p.y;
    float ww = widths[edge];
    float period = 4.0 * ww;
    float m = t - period * floor(t / period);
    float on = m < 2.0 * ww ? min(m, 2.0 * ww - m) : -min(m - 2.0 * ww, period - m);
    return lx_saturate(0.5 + on);
}

void main() {
    vec2 p = gl_FragCoord.xy;
    float hw = v_bounds.z * 0.5;
    float hh = v_bounds.w * 0.5;
    vec4 box = vec4(v_bounds.x + hw, v_bounds.y + hh, hw, hh);
    float a_o = lx_coverage(box, v_r, p);
    if (!(a_o > 0.0)) {
        o_color = vec4(0.0);
        return;
    }
    vec4 bg = lx_background(p, box);
    if ((v_flags & 1u) == 0u) {
        o_color = bg * a_o;
        return;
    }
    float a_i = lx_coverage(v_ibox, v_ir, p);
    float w_i = min(a_i, a_o);
    float w_b = a_o - w_i;
    vec4 o = bg * w_i;
    if (w_b > 0.0) {
        vec4 border = (v_flags & 2u) != 0u ? v_border * lx_dash(p) : v_border;
        o = o + (border + bg * (1.0 - border.a)) * w_b;
    }
    o_color = o;
}
