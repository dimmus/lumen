// A draw's source sample, with the area filter when the draw shrinks its source
// (lx::gfx::downsample_footprint_for, renderer.cppm "Downsampling"; gl_renderer.cppm's
// LUMEN_DOWNSAMPLE is the GLES 2 twin). Without `downsample` it is the one bilinear tap it
// always was, so 1:1 and magnified draws do not change.
//
// `ax`, `ay`: the source step (normalized) of one destination pixel along x and y; every tap
// is clamped to `bounds` (u_lo, v_lo, u_hi, v_hi), as the single tap always was.

// Texels k and k + 1 (texel k spans [k, k + 1]) of footprint [a, b] as one bilinear tap:
// its position between their centres at the ratio of their coverage, and the coverage sum
// as its weight. Zero weight past the footprint.
vec2 area_tap(float a, float b, float k) {
    float wa = clamp(min(k + 1.0, b) - max(k, a), 0.0, 1.0);
    float wb = clamp(min(k + 2.0, b) - max(k + 1.0, a), 0.0, 1.0);
    float w = wa + wb;
    return vec2(k + 0.5 + (w > 0.0 ? wb / w : 0.0), w);
}

// The taps along one axis, centre `c` and footprint `f` in texels: three pairs cover up to
// 5 texels (a footprint of 4, k_max_downsample). An axis not shrunk keeps the plain
// bilinear position.
void axis_taps(float c, float f, out vec2 t[3]) {
    if (f <= 1.015625) {
        t[0] = vec2(c, 1.0);
        t[1] = vec2(0.0);
        t[2] = vec2(0.0);
        return;
    }
    float a = c - 0.5 * f;
    float b = c + 0.5 * f;
    float k = floor(a);
    t[0] = area_tap(a, b, k);
    t[1] = area_tap(a, b, k + 2.0);
    t[2] = area_tap(a, b, k + 4.0);
}

vec4 sample_source(sampler2D tex, vec2 uv, vec4 bounds, vec2 ax, vec2 ay, int downsample) {
    if (downsample == 0)
        return texture(tex, clamp(uv, bounds.xy, bounds.zw));
    vec2 size = vec2(textureSize(tex, 0));
    // The buffer transforms are quarter turns: each source axis is one of ax, ay.
    vec2 f = min((abs(ax) + abs(ay)) * size, vec2(4.0));
    vec2 c = uv * size;
    vec2 tu[3];
    vec2 tv[3];
    axis_taps(c.x, f.x, tu);
    axis_taps(c.y, f.y, tv);
    vec4 sum = vec4(0.0);
    // `downsample` is a push constant: uniform control flow. The level is explicit (the
    // textures have one) rather than derived inside the loop.
    for (int j = 0; j < 3; ++j) {
        if (tv[j].y <= 0.0)
            continue;
        for (int i = 0; i < 3; ++i) {
            if (tu[i].y <= 0.0)
                continue;
            vec2 p = clamp(vec2(tu[i].x, tv[j].x) / size, bounds.xy, bounds.zw);
            sum += textureLod(tex, p, 0.0) * (tu[i].y * tv[j].y);
        }
    }
    return sum / ((tu[0].y + tu[1].y + tu[2].y) * (tv[0].y + tv[1].y + tv[2].y));
}
