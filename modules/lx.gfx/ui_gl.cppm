module;

#if defined(LUMEN_HAS_EGL)
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

import lx.foundation;

export module lx.gfx:ui_gl;

import std;

import :primitives;
import :atlas;
import :damage;
import :renderer;
import :gl_renderer;
import :ui_cpu;

export namespace lx::gfx {

// ── The UI primitive renderer, GLES 3.1 (ADR-010 §4.4, Phase 1) ─────────────────
//
// Draws one view's sorted primitive stream (`ui_stream`, the same input `ui_cpu_renderer`
// takes) into a framebuffer object of the current GLES 3.1 context, inside the damage rects,
// at pixel parity with the CPU reference (G10: within 3/255 per channel). The pixel rules
// are `:ui_cpu`'s, word for word — its header comment is the specification; the shaders
// below transcribe its functions (the same SDF, border, dash, gradient, the same four-sample
// erf shadow, the same Taylor sine for wavy underlines and gradient directions).
//
// Context. Any current GLES 3.1 context with at least one vertex-stage shader storage block:
// `egl_device` on a render node (Mesa answers its ES 2 request with the highest ES version
// the driver has), or a compositor's. Older contexts are refused (`gl_err::no_context`);
// a software GL_RENDERER is accepted and reported in `caps().software` — correct, but not a
// performance number.
//
// Instances. One buffer of `k_ui_gl_ring_slots` × `k_ui_gl_slot_bytes`, used as a ring: a
// frame writes its slot, a fence closes the slot, and the slot is reused three frames
// later once the fence has signalled. With GL_EXT_buffer_storage the buffer is mapped once,
// persistent and coherent, and the encoder gathers instances straight into it; otherwise each
// frame maps its slot's range with WRITE | INVALIDATE_RANGE | UNSYNCHRONIZED (the fence makes
// unsynchronized safe). All kinds pack into the slot, each batch contiguous and in sorted
// order; the vertex stage reads its kind's instances from the ring as a std430 `uint[]`
// at `u_base + gl_InstanceID · stride` (GLES has no base instance: the offset is a
// uniform per batch), or — `ui_gl_fetch::attributes`, for a context without vertex-stage
// storage blocks — as per-instance integer attributes with the offset in the pointers.
// Every draw is `glDrawArraysInstanced(GL_TRIANGLES, 0, 6, n)`, the unit
// quad from `gl_VertexID`, no vertex buffer.
//
// Coverage. The vertex stage turns each instance into its raster window — the pixels whose
// centre lies in the raster bounds and the content mask, `:ui_cpu`'s [⌈lo − ½⌉, ⌈hi − ½⌉)
// — and emits the quad on those whole-pixel edges, so the rasteriser's own edge rule never
// decides a pixel: every centre is ½ px inside. The content mask therefore costs no
// fragment work and no scissor; the scissor is the damage rect's alone. Fragments read
// `gl_FragCoord` (row 0 is the top, as in `gl_compositor`: the FBO is never flipped).
//
// Blending. GL_ONE, GL_ONE_MINUS_SRC_ALPHA into an 8-bit UNORM target in the encoded space
// (ADR-010 §4.5): exactly the CPU's premultiplied source-over with one byte rounding per
// primitive, instances blending in stream order. Dithering is off.
//
// Atlas. One immutable 2D texture per created page (GL_R8 for 0–3, GL_RGBA8 for 4–5),
// sized to the page; a page that grew is recreated and uploaded whole, otherwise only its
// dirty rects go up (glTexSubImage2D at the page's stride), and the atlas's dirty lists are
// cleared. Sampling is `texelFetch`: the exact nearest texel at 1:1 whole-pixel placement,
// the CPU's manual bilinear (clamped to the tile's texel centres) otherwise.
//
// Damage. Per damage rect: scissor, clear (when a clear colour is given), then every batch
// whose bounds meet the rect — the order and skip rule of `gl_compositor::composite_rects`
// and `ui_cpu_renderer`. Surfaces (`surface` refs) go through `gl_compositor::composite_into`
// when one is set, exactly as the compositor draws a `blit_command`.
//
// GPU timing. GL_EXT_disjoint_timer_query when it works: `initialize` times eight clears of a
// 256² target once, and a timer that reads 0 for that (SVGA3D exposes the extension and
// answers every query with 0) is not used. Otherwise `wait_gpu_time` reports "fence-wall":
// the wall time from the frame's flush to its fence signalling.
//
// Programs. Five, compiled once by `initialize`: shadow, quad, mono sprite (also `path`),
// poly sprite, underline. With GL_NUM_PROGRAM_BINARY_FORMATS > 0 each linked program's
// binary is kept under $XDG_CACHE_HOME/lumen/gl-program-<hash>.bin, the hash over
// GL_RENDERER, GL_VERSION and the program's sources; a cached binary that fails to load is
// recompiled and rewritten.
//
// Threads: the affinity that owns the context (render in the compositor, ui in a client).
// Nothing in `draw` allocates.

inline constexpr unsigned k_ui_gl_ring_slots = 3;
inline constexpr unsigned k_ui_gl_slot_bytes = 2u << 20u;

/// How `wait_gpu_time` measured.
enum class ui_gl_timing : std::uint8_t {
    none,        ///< no frame was timed
    timer_query, ///< GL_EXT_disjoint_timer_query, GL_TIME_ELAPSED around the frame's GPU work
    fence_wall,  ///< no timer query: wall time from the frame's flush to its fence signalling
};

/// How the vertex stage reads its instance.
enum class ui_gl_fetch : std::uint8_t {
    /// The ring as a std430 storage buffer, `u_base + gl_InstanceID · stride` words (ADR-010
    /// §4.4). The default.
    storage_buffer,
    /// Per-instance integer vertex attributes (divisor 1, four words each), the batch's
    /// offset in the attribute pointers. Taken automatically when the context has no
    /// vertex-stage storage block (GLES 3.1 allows none); on SVGA3D the two measure the same
    /// (S3: 8.5 ms GPU either way).
    attributes,
};

/// Feature switches for `initialize` (all on by default; tests turn them off to prove the
/// fallback paths draw the same pixels).
struct ui_gl_options {
    ui_gl_fetch fetch = ui_gl_fetch::storage_buffer;
    bool persistent_ring = true; ///< use GL_EXT_buffer_storage when present
    bool timer_query = true;     ///< use GL_EXT_disjoint_timer_query when present
    bool program_cache = true;   ///< cache program binaries when the driver has a format
};

/// What the context offers and what `initialize` chose.
struct ui_gl_caps {
    ui_gl_fetch fetch = ui_gl_fetch::storage_buffer;
    char renderer[128]{};
    char version[96]{};
    int major = 0;
    int minor = 0;
    /// llvmpipe, softpipe, swrast: correct, never a performance number.
    bool software = false;
    bool buffer_storage = false;       ///< GL_EXT_buffer_storage exposed
    bool persistent_ring = false;      ///< the ring is one persistent coherent mapping
    bool disjoint_timer_query = false; ///< GL_EXT_disjoint_timer_query exposed
    /// Exposed, but a calibration frame of real work measured 0 ns (SVGA3D does this): not
    /// used, `wait_gpu_time` falls back to the fence.
    bool timer_query_reads_zero = false;
    bool timer_query = false;          ///< used by `wait_gpu_time`
    int program_binary_formats = 0;    ///< GL_NUM_PROGRAM_BINARY_FORMATS
    bool program_cache = false;        ///< binaries are read from / written to the cache
};

/// Program setup in `initialize`: cold (compiled) or warm (from the binary cache).
struct ui_gl_setup_stats {
    std::uint64_t program_ns = 0; ///< every program compiled + linked or loaded (CLOCK_MONOTONIC)
    unsigned programs = 0;
    unsigned from_cache = 0;      ///< programs loaded from a cached binary
    unsigned written = 0;         ///< binaries written to the cache
};

struct ui_gl_draw_stats {
    unsigned rects = 0;               ///< damage rects drawn
    unsigned batches_drawn = 0;       ///< (batch, rect) pairs that met
    unsigned batches_skipped = 0;     ///< (batch, rect) pairs whose bounds missed
    unsigned draw_calls = 0;          ///< instanced draws plus surface draws
    unsigned primitives_encoded = 0;  ///< instances written to the ring
    unsigned primitives_skipped = 0;  ///< bad index, missing or mismatched page, bad uv, no compositor
    unsigned overflow_drops = 0;      ///< primitives of batches that did not fit the slot (dropped)
    std::uint64_t instance_bytes = 0; ///< bytes written to the slot
    unsigned atlas_uploads = 0;       ///< glTexSubImage2D calls
    std::uint64_t atlas_upload_bytes = 0;
    unsigned slot = 0;                ///< ring slot of this frame
    std::uint64_t slot_wait_ns = 0;   ///< waiting for the slot's previous fence
};

struct ui_gl_gpu_time {
    ui_gl_timing source = ui_gl_timing::none;
    std::uint64_t ns = 0;
    /// The timer query was disjoint (GL_GPU_DISJOINT_EXT): `ns` is not to be trusted.
    bool disjoint = false;
};

/// A framebuffer object over a GL_RGBA8 renderbuffer: the target the tests and the bench
/// draw into (encoded values, no sRGB conversion). Owned by the current context.
class ui_gl_target {
public:
    ui_gl_target() = default;
    ~ui_gl_target();
    ui_gl_target(const ui_gl_target&) = delete;
    ui_gl_target& operator=(const ui_gl_target&) = delete;
    ui_gl_target(ui_gl_target&& other) noexcept;
    ui_gl_target& operator=(ui_gl_target&& other) noexcept;

    /// Needs a current context.
    [[nodiscard]] static lx::result<ui_gl_target> create(unsigned width, unsigned height);

    [[nodiscard]] bool valid() const { return framebuffer_ != 0; }
    [[nodiscard]] unsigned framebuffer() const { return framebuffer_; }
    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }

    /// Reads the target back as XRGB8888 / ARGB8888 words (B, G, R, A in memory), rows top
    /// first, `stride_px` words apart (0: the width); `opaque` writes 255 in the fourth
    /// byte (an XRGB target). Waits for the GPU: tests and the bench only.
    [[nodiscard]] lx::result<void> read_back(std::span<std::uint32_t> out, unsigned stride_px = 0,
                                             bool opaque = true) const;

private:
    void destroy();

    unsigned renderbuffer_ = 0;
    unsigned framebuffer_ = 0;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

class ui_gl_renderer {
public:
    ui_gl_renderer() = default;
    /// Calls `shutdown`: the context must be current.
    ~ui_gl_renderer();
    ui_gl_renderer(const ui_gl_renderer&) = delete;
    ui_gl_renderer& operator=(const ui_gl_renderer&) = delete;

    /// Compiles the programs (or loads them from the binary cache) and creates the ring, in
    /// the current context. Refuses a context below GLES 3.1 or one without a vertex-stage
    /// shader storage block.
    [[nodiscard]] lx::result<void> initialize(const ui_gl_options& options = {});
    void shutdown();
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const ui_gl_caps& caps() const { return caps_; }
    [[nodiscard]] const ui_gl_setup_stats& setup_stats() const { return setup_; }

    /// Where `surface` refs' textures are registered (same context). Borrowed; null skips
    /// surfaces.
    void set_surface_compositor(gl_compositor* compositor) { compositor_ = compositor; }

    /// Times each frame's GPU work (a timer query, or the fence when there is none), read
    /// with `wait_gpu_time`. Off by default.
    void set_gpu_timing(bool on) { timing_ = on; }

    /// Draws `stream` into `framebuffer` (`width` × `height`, row 0 on top) inside `damage`,
    /// clearing each damage rect to `clear` (premultiplied, encoded) first when given. Uploads
    /// the atlas's dirty pages first and clears its dirty lists (`atlas` may be null when the
    /// stream has no sprites). An empty region draws nothing and submits nothing. Flushes;
    /// never waits for the GPU (except for a ring slot still in flight three frames on).
    [[nodiscard]] lx::result<ui_gl_draw_stats> draw(unsigned framebuffer, unsigned width,
                                                    unsigned height, const ui_stream& stream,
                                                    atlas* atlas, const damage_region& damage,
                                                    std::optional<lx::color> clear = std::nullopt);
    [[nodiscard]] lx::result<ui_gl_draw_stats> draw(const ui_gl_target& target,
                                                    const ui_stream& stream, atlas* atlas,
                                                    const damage_region& damage,
                                                    std::optional<lx::color> clear = std::nullopt) {
        return draw(target.framebuffer(), target.width(), target.height(), stream, atlas,
                    damage, clear);
    }

    /// The GPU time of the last timed frame; waits for it. `none` when timing is off or no
    /// frame was drawn since the last call.
    [[nodiscard]] lx::result<ui_gl_gpu_time> wait_gpu_time();

private:
    enum program_index : unsigned { p_shadow, p_quad, p_mono, p_poly, p_underline, p_count };
    struct program {
        unsigned id = 0;
        int u_base = -1;
        int u_target = -1;
    };
    struct page_texture {
        unsigned name = 0;
        unsigned size = 0;
    };

    [[nodiscard]] lx::result<void> build_program(program_index index);
    [[nodiscard]] lx::result<void> upload_atlas(atlas& a, ui_gl_draw_stats& stats);
    [[nodiscard]] std::size_t encode(const ui_stream& stream, const atlas* a, std::byte* slot,
                                     ui_gl_draw_stats& stats);

    ui_gl_caps caps_{};
    ui_gl_setup_stats setup_{};
    ui_gl_options options_{};
    program programs_[p_count]{};
    unsigned buffer_ = 0;
    unsigned vertex_array_ = 0; ///< attribute fetch: divisors and pointers live here
    void* mapping_ = nullptr; ///< the persistent mapping, or null
    void* fences_[k_ui_gl_ring_slots]{};
    unsigned queries_[k_ui_gl_ring_slots]{};
    unsigned frame_ = 0;
    int timed_slot_ = -1; ///< slot of the last timed frame, until `wait_gpu_time`
    std::uint64_t timed_flush_ns_ = 0;
    page_texture pages_[k_atlas_pages]{};
    const atlas* atlas_seen_ = nullptr;
    /// Per stream batch: word offset in the slot and instances written (0: not drawn).
    std::uint32_t batch_word_[k_max_batches]{};
    std::uint32_t batch_count_[k_max_batches]{};
    gl_compositor* compositor_ = nullptr;
    // GL_EXT_buffer_storage / GL_EXT_disjoint_timer_query entry points.
    void* buffer_storage_ = nullptr;
    void* gen_queries_ = nullptr;
    void* delete_queries_ = nullptr;
    void* begin_query_ = nullptr;
    void* end_query_ = nullptr;
    void* query_ui64_ = nullptr;
    bool ready_ = false;
    bool timing_ = false;
};

} // namespace lx::gfx

// ── Shaders ─────────────────────────────────────────────────────────────────────

namespace lx::gfx::ui_gl_detail {

#if defined(LUMEN_HAS_EGL)

constexpr const char* k_header = R"(#version 310 es
precision highp float;
precision highp int;
)";

// Deterministic helpers shared by both stages: `ui_math::sin` (the CPU's Taylor series after
// the same range reduction), the sRGB curves and Oklab of `ui_raster::to_oklab`/`from_oklab`.
constexpr const char* k_math = R"(
const float LX_PI = 3.14159265358979323846;
float lx_saturate(float v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
float lx_sin(float x) {
    float turns = floor(x / (2.0 * LX_PI) + 0.5);
    x = x - turns * (2.0 * LX_PI);
    if (x > LX_PI * 0.5)
        x = LX_PI - x;
    else if (x < -LX_PI * 0.5)
        x = -LX_PI - x;
    float x2 = x * x;
    float term = x;
    float sum = x;
    for (int k = 2; k <= 12; k += 2) {
        term = -term * x2 / float(k * (k + 1));
        sum = sum + term;
    }
    return sum;
}
float lx_cos(float x) { return lx_sin(x + LX_PI * 0.5); }
float lx_srgb_decode(float v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
float lx_srgb_encode(float v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
float lx_cbrt(float x) { return x == 0.0 ? 0.0 : (x < 0.0 ? -pow(-x, 1.0 / 3.0) : pow(x, 1.0 / 3.0)); }
vec4 lx_to_oklab(vec4 c) {
    float inv = c.a > 0.0 ? 1.0 / c.a : 0.0;
    float r = lx_srgb_decode(lx_saturate(c.r * inv));
    float g = lx_srgb_decode(lx_saturate(c.g * inv));
    float b = lx_srgb_decode(lx_saturate(c.b * inv));
    float l = lx_cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    float m = lx_cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    float s = lx_cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return vec4(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s, c.a);
}
vec4 lx_from_oklab(vec4 lab) {
    float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;
    float r = lx_saturate(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
    float g = lx_saturate(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
    float b = lx_saturate(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
    float a = lx_saturate(lab.w);
    return vec4(lx_srgb_encode(r) * a, lx_srgb_encode(g) * a, lx_srgb_encode(b) * a, a);
}
// A rounded box as (centre x, centre y, half w, half h), half w < 0 when empty; radii
// top-left, top-right, bottom-right, bottom-left, clamped to [0, min(half size)].
vec4 lx_make_box(vec4 rect, vec4 radii, out vec4 r) {
    r = vec4(0.0);
    if (!(rect.z > 0.0) || !(rect.w > 0.0))
        return vec4(0.0, 0.0, -1.0, -1.0);
    float hw = rect.z * 0.5;
    float hh = rect.w * 0.5;
    float limit = min(hw, hh);
    r = min(max(radii, vec4(0.0)), vec4(limit));
    return vec4(rect.x + hw, rect.y + hh, hw, hh);
}
)";

// The vertex stage: instance fetch, the raster window and the whole-pixel quad. Words come
// from per-instance integer attributes (four per attribute, the batch's offset in the
// attribute pointers) or from the ring as a storage buffer (`u_base` + instance · stride).
// `i` is a constant at every call site, so the attribute selection folds away.
constexpr const char* k_vertex_common = R"(
uniform uint u_base;
uniform vec2 u_target;
#ifdef LX_ATTRIBUTES
layout(location = 0) in uvec4 i_w0;
layout(location = 1) in uvec4 i_w1;
layout(location = 2) in uvec4 i_w2;
#if LX_VEC4S > 3
layout(location = 3) in uvec4 i_w3;
#endif
#if LX_VEC4S > 4
layout(location = 4) in uvec4 i_w4;
#endif
uint inst_word(uint i) {
    uvec4 v = i < 4u ? i_w0 : (i < 8u ? i_w1 : i_w2);
#if LX_VEC4S > 3
    if (i >= 12u)
        v = i_w3;
#endif
#if LX_VEC4S > 4
    if (i >= 16u)
        v = i_w4;
#endif
    return v[i & 3u];
}
#else
layout(std430, binding = 0) readonly buffer lx_instances { uint lx_words[]; };
uint inst_word(uint i) { return lx_words[u_base + uint(gl_InstanceID) * LX_STRIDE + i]; }
#endif
float inst_float(uint i) { return uintBitsToFloat(inst_word(i)); }
vec4 inst_vec4(uint i) {
    return vec4(inst_float(i), inst_float(i + 1u), inst_float(i + 2u), inst_float(i + 3u));
}
vec4 inst_half4(uint i) { return vec4(unpackHalf2x16(inst_word(i)), unpackHalf2x16(inst_word(i + 1u))); }

float lx_clamp_px(float v) {
    if (!(v >= -1.0e6))
        return -1.0e6;
    if (!(v <= 1.0e6))
        return 1.0e6;
    return v;
}
// The pixels whose centre lies in [lo, hi): [ceil(lo - 1/2), ceil(hi - 1/2)).
vec2 lx_centres(float lo, float hi) {
    return vec2(lx_clamp_px(ceil(lo - 0.5)), lx_clamp_px(ceil(hi - 0.5)));
}
// x0, y0, x1, y1 of the pixels whose centre is in `r` and in `clip` (no mask when empty),
// within the target.
vec4 lx_window(vec4 r, vec4 clip) {
    vec2 xs = lx_centres(r.x, r.x + r.z);
    vec2 ys = lx_centres(r.y, r.y + r.w);
    vec4 w = vec4(xs.x, ys.x, xs.y, ys.y);
    if (clip.z > 0.0 && clip.w > 0.0) {
        vec2 cx = lx_centres(clip.x, clip.x + clip.z);
        vec2 cy = lx_centres(clip.y, clip.y + clip.w);
        w = vec4(max(w.x, cx.x), max(w.y, cy.x), min(w.z, cx.y), min(w.w, cy.y));
    }
    return vec4(max(w.xy, vec2(0.0)), min(w.zw, u_target));
}
void lx_cull() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); }
// Places this vertex of the window's two triangles; false (and culled) when it is empty.
bool lx_place(vec4 w) {
    if (!(w.z > w.x) || !(w.w > w.y)) {
        lx_cull();
        return false;
    }
    int v = gl_VertexID;
    vec2 corner = vec2((v == 1 || v == 4 || v == 5) ? 1.0 : 0.0, (v == 2 || v == 3 || v == 5) ? 1.0 : 0.0);
    vec2 p = mix(w.xy, w.zw, corner);
    gl_Position = vec4(p / u_target * 2.0 - 1.0, 0.0, 1.0);
    return true;
}
)";

constexpr const char* k_fragment_common = R"(
layout(location = 0) out vec4 o_color;

float lx_pick(vec4 r, vec2 d) { return d.x < 0.0 ? (d.y < 0.0 ? r.x : r.w) : (d.y < 0.0 ? r.y : r.z); }
// GPUI's rounded-box signed distance (negative inside), as `ui_raster::sdf`.
float lx_sdf(vec4 box, vec4 r, vec2 p) {
    vec2 d = p - box.xy;
    float rr = lx_pick(r, d);
    float qx = abs(d.x) - box.z + rr;
    float qy = abs(d.y) - box.w + rr;
    if (rr == 0.0)
        return max(qx, qy);
    float mx = max(qx, 0.0);
    float my = max(qy, 0.0);
    float len = mx == 0.0 ? my : (my == 0.0 ? mx : sqrt(mx * mx + my * my));
    return len + min(max(qx, qy), 0.0) - rr;
}
float lx_coverage(vec4 box, vec4 r, vec2 p) {
    return box.z < 0.0 ? 0.0 : lx_saturate(0.5 - lx_sdf(box, r, p));
}
)";

// ── quad ──

constexpr const char* k_quad_vs = R"(
flat out vec4 v_bounds;
flat out vec4 v_r;
flat out vec4 v_ibox;
flat out vec4 v_ir;
flat out vec4 v_widths;
flat out vec4 v_bg0;
flat out vec4 v_bg1;
flat out vec4 v_border;
flat out vec4 v_grad;
flat out vec4 v_lab0;
flat out vec4 v_lab1;
flat out uint v_flags;
void main() {
    vec4 bounds = inst_vec4(0u);
    vec4 r;
    vec4 box = lx_make_box(bounds, inst_half4(8u), r);
    if (box.z < 0.0) {
        lx_cull();
        return;
    }
    if (!lx_place(lx_window(bounds, inst_vec4(4u))))
        return;
    vec4 widths = max(inst_half4(10u), vec4(0.0)); // top, right, bottom, left
    uint flags = 0u;
    vec4 ir = vec4(0.0);
    vec4 ibox = vec4(0.0, 0.0, -1.0, -1.0);
    bool border = widths.x > 0.0 || widths.y > 0.0 || widths.z > 0.0 || widths.w > 0.0;
    if (border) {
        flags |= 1u;
        vec4 inner_radii = vec4(max(r.x - max(widths.x, widths.w), 0.0),
                                max(r.y - max(widths.x, widths.y), 0.0),
                                max(r.z - max(widths.z, widths.y), 0.0),
                                max(r.w - max(widths.z, widths.w), 0.0));
        ibox = lx_make_box(vec4(bounds.x + widths.w, bounds.y + widths.x,
                                bounds.z - widths.w - widths.y, bounds.w - widths.x - widths.z),
                           inner_radii, ir);
    }
    uint style = inst_word(18u); // angle f16 | border_style << 16 | gradient_space << 24
    if (border && ((style >> 16u) & 0xFFu) == 1u)
        flags |= 2u;
    v_bg0 = inst_half4(12u);
    v_bg1 = inst_half4(14u);
    v_border = inst_half4(16u);
    v_grad = vec4(1.0, 0.0, 0.0, 0.0);
    v_lab0 = vec4(0.0);
    v_lab1 = vec4(0.0);
    if (inst_word(12u) != inst_word(14u) || inst_word(13u) != inst_word(15u)) {
        flags |= 4u;
        float theta = unpackHalf2x16(style & 0xFFFFu).x * (LX_PI / 180.0);
        float dx = lx_cos(theta);
        float dy = lx_sin(theta);
        float len = abs(bounds.z * dx) + abs(bounds.w * dy);
        v_grad = vec4(dx, dy, len > 0.0 ? 1.0 / len : 0.0, 0.0);
        if (((style >> 24u) & 0xFFu) == 1u) {
            flags |= 8u;
            v_lab0 = lx_to_oklab(v_bg0);
            v_lab1 = lx_to_oklab(v_bg1);
        }
    }
    v_bounds = bounds;
    v_r = r;
    v_ibox = ibox;
    v_ir = ir;
    v_widths = widths;
    v_flags = flags;
}
)";

constexpr const char* k_quad_fs = R"(
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
)";

// ── shadow ──

constexpr const char* k_shadow_vs = R"(
flat out vec4 v_box;
flat out vec4 v_r;
flat out vec4 v_elem;
flat out vec4 v_er;
flat out vec4 v_color;
flat out vec4 v_sigma; // sigma, sqrt(1/2) / sigma, 1 / (sqrt(2 pi) sigma), -1 / (2 sigma^2)
flat out uint v_flags; // 1 inset, 2 hard
void main() {
    vec4 bounds = inst_vec4(0u);
    vec4 radii = inst_half4(8u);
    float sigma = max(inst_float(12u), 0.0);
    float sp = inst_float(13u);
    bool inset = (inst_word(14u) & 1u) != 0u;
    float reach = inset ? 0.0 : 3.0 * sigma + sp;
    vec4 raster = vec4(bounds.x - reach, bounds.y - reach, bounds.z + 2.0 * reach, bounds.w + 2.0 * reach);
    if (!lx_place(lx_window(raster, inst_vec4(4u))))
        return;
    uint flags = inset ? 1u : 0u;
    bool hard = sigma < 1.0e-4;
    if (hard)
        flags |= 2u;
    vec4 r;
    vec4 er = vec4(0.0);
    vec4 box;
    vec4 elem = vec4(0.0, 0.0, -1.0, -1.0);
    if (inset) {
        elem = lx_make_box(bounds, radii, er);
        box = lx_make_box(vec4(bounds.x + sp, bounds.y + sp, bounds.z - 2.0 * sp, bounds.w - 2.0 * sp),
                          max(radii - sp, vec4(0.0)), r);
    } else {
        box = lx_make_box(vec4(bounds.x - sp, bounds.y - sp, bounds.z + 2.0 * sp, bounds.w + 2.0 * sp),
                          max(radii + sp, vec4(0.0)), r);
    }
    v_sigma = hard ? vec4(sigma, 0.0, 0.0, 0.0)
                   : vec4(sigma, 0.70710678118 / sigma, 1.0 / (2.50662827463 * sigma),
                          -1.0 / (2.0 * sigma * sigma));
    v_box = box;
    v_r = r;
    v_elem = elem;
    v_er = er;
    v_color = inst_half4(10u);
    v_flags = flags;
}
)";

constexpr const char* k_shadow_fs = R"(
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
)";

// ── sprites (mono and path: R8; poly with LX_POLY: RGBA8) ──

constexpr const char* k_sprite_vs = R"(
flat out vec4 v_bounds;
flat out vec4 v_tile;  // u0, v0, u1, v1 (texels)
flat out vec4 v_color;
flat out vec4 v_mbox;  // poly: the rounded mask
flat out vec4 v_mr;
flat out uint v_flags; // 1 nearest, 2 grayscale, 4 rounded
void main() {
    vec4 bounds = inst_vec4(0u);
    if (!lx_place(lx_window(bounds, inst_vec4(4u))))
        return;
    uint a = inst_word(8u);
    uint b = inst_word(9u);
    vec4 tile = vec4(float(a & 0xFFFFu), float(a >> 16u), float(b & 0xFFFFu), float(b >> 16u));
    uint flags = 0u;
    if (bounds.x == floor(bounds.x) && bounds.y == floor(bounds.y) && bounds.z == tile.z - tile.x &&
        bounds.w == tile.w - tile.y)
        flags |= 1u;
    vec4 mr = vec4(0.0);
    vec4 mbox = vec4(0.0, 0.0, -1.0, -1.0);
#ifdef LX_POLY
    if (((inst_word(10u) >> 8u) & 1u) != 0u)
        flags |= 2u;
    vec4 radii = inst_half4(13u);
    if (radii.x > 0.0 || radii.y > 0.0 || radii.z > 0.0 || radii.w > 0.0) {
        flags |= 4u;
        mbox = lx_make_box(bounds, radii, mr);
    }
#endif
    v_bounds = bounds;
    v_tile = tile;
    v_color = inst_half4(11u);
    v_mbox = mbox;
    v_mr = mr;
    v_flags = flags;
}
)";

constexpr const char* k_sprite_fs = R"(
flat in vec4 v_bounds;
flat in vec4 v_tile;
flat in vec4 v_color;
flat in vec4 v_mbox;
flat in vec4 v_mr;
flat in uint v_flags;
uniform highp sampler2D u_page;

void main() {
    vec2 p = gl_FragCoord.xy;
    vec4 texel;
    if ((v_flags & 1u) != 0u) {
        texel = texelFetch(u_page, ivec2(v_tile.xy) + (ivec2(floor(p)) - ivec2(v_bounds.xy)), 0);
    } else {
        // `ui_raster::sample`: bilinear at (u - 1/2, v - 1/2), clamped to the tile's texel centres.
        float ku = (v_tile.z - v_tile.x) / v_bounds.z;
        float kv = (v_tile.w - v_tile.y) / v_bounds.w;
        float su = clamp(v_tile.x + (p.x - v_bounds.x) * ku - 0.5, v_tile.x, v_tile.z - 1.0);
        float sv = clamp(v_tile.y + (p.y - v_bounds.y) * kv - 0.5, v_tile.y, v_tile.w - 1.0);
        float fu = floor(su);
        float fv = floor(sv);
        float wx = su - fu;
        float wy = sv - fv;
        int i0 = int(fu);
        int j0 = int(fv);
        int i1 = i0 + 1 < int(v_tile.z) ? i0 + 1 : i0;
        int j1 = j0 + 1 < int(v_tile.w) ? j0 + 1 : j0;
        vec4 p00 = texelFetch(u_page, ivec2(i0, j0), 0);
        vec4 p10 = texelFetch(u_page, ivec2(i1, j0), 0);
        vec4 p01 = texelFetch(u_page, ivec2(i0, j1), 0);
        vec4 p11 = texelFetch(u_page, ivec2(i1, j1), 0);
        vec4 top = p00 + (p10 - p00) * wx;
        vec4 bottom = p01 + (p11 - p01) * wx;
        texel = top + (bottom - top) * wy;
    }
#ifdef LX_POLY
    vec4 c = texel;
    if ((v_flags & 2u) != 0u) {
        float l = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        c.rgb = vec3(l);
    }
    vec4 src = c * v_color;
    if ((v_flags & 4u) != 0u)
        src = src * lx_coverage(v_mbox, v_mr, p);
    o_color = src;
#else
    o_color = v_color * texel.r;
#endif
}
)";

// ── underline ──

constexpr const char* k_underline_vs = R"(
flat out vec4 v_bounds;
flat out vec4 v_color;
flat out vec4 v_line; // thickness, thickness / 2, omega, centre y
flat out uint v_wavy;
void main() {
    vec4 bounds = inst_vec4(0u);
    uint tw = inst_word(10u); // thickness f16 | wavy << 16
    float t = max(unpackHalf2x16(tw & 0xFFFFu).x, 0.0);
    if (!(t > 0.0)) {
        lx_cull();
        return;
    }
    if (!lx_place(lx_window(bounds, inst_vec4(4u))))
        return;
    v_bounds = bounds;
    v_color = inst_half4(8u);
    v_line = vec4(t, t * 0.5, 2.0 * LX_PI / (4.0 * t), bounds.y + bounds.w * 0.5);
    v_wavy = tw >> 16u;
}
)";

constexpr const char* k_underline_fs = R"(
flat in vec4 v_bounds;
flat in vec4 v_color;
flat in vec4 v_line;
flat in uint v_wavy;
void main() {
    vec2 p = gl_FragCoord.xy;
    float dist;
    if (v_wavy != 0u) {
        float phase = (p.x - v_bounds.x) * v_line.z;
        float fy = v_line.w + v_line.x * lx_sin(phase);
        float slope = v_line.x * v_line.z * lx_cos(phase);
        dist = abs(p.y - fy) / sqrt(1.0 + slope * slope);
    } else {
        dist = abs(p.y - v_line.w);
    }
    float cov = lx_saturate(v_line.y - dist + 0.5);
    o_color = cov > 0.0 ? v_color * cov : vec4(0.0);
}
)";

struct program_source {
    const char* name;
    const char* defines; ///< after the header, both stages
    const char* vs;
    const char* fs;
    unsigned vec4s; ///< instance size in 16-byte attributes
};

/// By `program_index`.
constexpr program_source k_programs[] = {
    {"shadow", "#define LX_STRIDE 16u\n#define LX_VEC4S 4\n", k_shadow_vs, k_shadow_fs, 4},
    {"quad", "#define LX_STRIDE 20u\n#define LX_VEC4S 5\n", k_quad_vs, k_quad_fs, 5},
    {"mono_sprite", "#define LX_STRIDE 16u\n#define LX_VEC4S 4\n", k_sprite_vs, k_sprite_fs, 4},
    {"poly_sprite", "#define LX_STRIDE 16u\n#define LX_VEC4S 4\n#define LX_POLY 1\n", k_sprite_vs,
     k_sprite_fs, 4},
    {"underline", "#define LX_STRIDE 12u\n#define LX_VEC4S 3\n", k_underline_vs, k_underline_fs, 3},
};

constexpr const char* k_fetch_attributes = "#define LX_ATTRIBUTES 1\n";
constexpr const char* k_fetch_storage = "\n";

[[nodiscard]] inline bool has_extension(const char* extensions, const char* name) {
    if (!extensions || !name)
        return false;
    const std::size_t len = std::strlen(name);
    for (const char* p = std::strstr(extensions, name); p; p = std::strstr(p + len, name)) {
        const bool start_ok = p == extensions || p[-1] == ' ';
        const bool end_ok = p[len] == ' ' || p[len] == '\0';
        if (start_ok && end_ok)
            return true;
    }
    return false;
}

[[nodiscard]] inline bool software_renderer(const char* renderer) {
    static const char* const names[] = {"llvmpipe", "softpipe", "swrast", "SWR", "Software"};
    for (const char* name : names)
        if (std::strstr(renderer, name))
            return true;
    return false;
}

[[nodiscard]] inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/// FNV-1a, 64 bit.
inline void hash_bytes(std::uint64_t& h, const char* s) {
    for (; *s; ++s) {
        h ^= static_cast<unsigned char>(*s);
        h *= 0x100000001B3ull;
    }
    h ^= 0xFFu; // a separator, so ("ab", "c") and ("a", "bc") differ
    h *= 0x100000001B3ull;
}

/// $XDG_CACHE_HOME/lumen (or ~/.cache/lumen), created; false when neither is set or the
/// directory cannot be made.
[[nodiscard]] inline bool cache_dir(char (&out)[512]) {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    int n = 0;
    if (xdg && *xdg) {
        n = std::snprintf(out, sizeof out, "%s", xdg);
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home)
            return false;
        n = std::snprintf(out, sizeof out, "%s/.cache", home);
    }
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof out - 16)
        return false;
    ::mkdir(out, 0700);
    std::strcat(out, "/lumen");
    if (::mkdir(out, 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

constexpr std::uint32_t k_binary_magic = 0x4C474250u; // "PBGL"

[[nodiscard]] inline unsigned compile(GLenum kind, const char* const* parts, GLsizei count) {
    const GLuint shader = glCreateShader(kind);
    if (!shader)
        return 0;
    glShaderSource(shader, count, parts, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        // Kept for whoever debugs a driver: the log goes to stderr once, at start-up.
        char log[2048];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof log, &len, log);
        std::fprintf(stderr, "lx.gfx ui_gl: shader compile failed:\n%.*s\n", static_cast<int>(len), log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

#endif // LUMEN_HAS_EGL

} // namespace lx::gfx::ui_gl_detail

// ── ui_gl_target ────────────────────────────────────────────────────────────────

lx::gfx::ui_gl_target::~ui_gl_target() { destroy(); }

lx::gfx::ui_gl_target::ui_gl_target(ui_gl_target&& other) noexcept { *this = std::move(other); }

lx::gfx::ui_gl_target& lx::gfx::ui_gl_target::operator=(ui_gl_target&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    renderbuffer_ = other.renderbuffer_;
    framebuffer_ = other.framebuffer_;
    width_ = other.width_;
    height_ = other.height_;
    other.renderbuffer_ = 0;
    other.framebuffer_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    return *this;
}

void lx::gfx::ui_gl_target::destroy() {
#if defined(LUMEN_HAS_EGL)
    if (framebuffer_)
        glDeleteFramebuffers(1, &framebuffer_);
    if (renderbuffer_)
        glDeleteRenderbuffers(1, &renderbuffer_);
#endif
    framebuffer_ = 0;
    renderbuffer_ = 0;
    width_ = 0;
    height_ = 0;
}

lx::result<lx::gfx::ui_gl_target> lx::gfx::ui_gl_target::create(unsigned width, unsigned height) {
#if defined(LUMEN_HAS_EGL)
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return lx::invalid_argument("ui_gl_target::create: bad size");
    ui_gl_target t;
    t.width_ = width;
    t.height_ = height;
    glGenRenderbuffers(1, &t.renderbuffer_);
    glBindRenderbuffer(GL_RENDERBUFFER, t.renderbuffer_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, static_cast<GLsizei>(width),
                          static_cast<GLsizei>(height));
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &t.framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, t.framebuffer_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.renderbuffer_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR)
        return lx::make_error(lx::gl_err::target_failed, "ui_gl_target: RGBA8 FBO incomplete");
    return t;
#else
    (void)width;
    (void)height;
    return lx::not_implemented("lx::gfx::ui_gl_target::create");
#endif
}

lx::result<void> lx::gfx::ui_gl_target::read_back(std::span<std::uint32_t> out, unsigned stride_px,
                                                  bool opaque) const {
#if defined(LUMEN_HAS_EGL)
    if (!valid())
        return lx::not_implemented("lx::gfx::ui_gl_target::read_back");
    if (stride_px == 0)
        stride_px = width_;
    if (stride_px < width_ ||
        out.size() < static_cast<std::size_t>(stride_px) * (height_ - 1u) + width_)
        return lx::invalid_argument("ui_gl_target::read_back: destination too small");
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(stride_px));
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_), GL_RGBA,
                 GL_UNSIGNED_BYTE, out.data());
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return lx::make_error(lx::gl_err::draw_failed, "ui_gl_target: glReadPixels failed");
    // R, G, B, A bytes to the B, G, R, A of an XRGB / ARGB word. Row 0 is the top (window
    // y = 0 is memory row 0 of the FBO; nothing is flipped anywhere).
    for (unsigned y = 0; y < height_; ++y) {
        std::uint32_t* row = out.data() + static_cast<std::size_t>(y) * stride_px;
        for (unsigned x = 0; x < width_; ++x) {
            const std::uint32_t w = row[x];
            const std::uint32_t a = opaque ? 0xFFu : (w >> 24u);
            row[x] = (a << 24u) | ((w & 0xFFu) << 16u) | (w & 0xFF00u) | ((w >> 16u) & 0xFFu);
        }
    }
    return {};
#else
    (void)out;
    (void)stride_px;
    (void)opaque;
    return lx::not_implemented("lx::gfx::ui_gl_target::read_back");
#endif
}

// ── ui_gl_renderer ──────────────────────────────────────────────────────────────

lx::gfx::ui_gl_renderer::~ui_gl_renderer() { shutdown(); }

lx::result<void> lx::gfx::ui_gl_renderer::build_program(program_index index) {
#if defined(LUMEN_HAS_EGL)
    using namespace ui_gl_detail;
    const program_source& src = k_programs[index];
    const char* fetch = caps_.fetch == ui_gl_fetch::attributes ? k_fetch_attributes : k_fetch_storage;
    const char* vs[] = {k_header, fetch, src.defines, k_math, k_vertex_common, src.vs};
    const char* fs[] = {k_header, fetch, src.defines, k_math, k_fragment_common, src.fs};

    char path[640]{};
    bool cacheable = false;
    if (caps_.program_cache) {
        char dir[512];
        if (cache_dir(dir)) {
            std::uint64_t h = 0xCBF29CE484222325ull;
            hash_bytes(h, caps_.renderer);
            hash_bytes(h, caps_.version);
            for (const char* part : vs)
                hash_bytes(h, part);
            for (const char* part : fs)
                hash_bytes(h, part);
            std::snprintf(path, sizeof path, "%s/gl-program-%016llx.bin", dir,
                          static_cast<unsigned long long>(h));
            cacheable = true;
        }
    }

    GLuint prog = 0;
    if (cacheable) {
        if (std::FILE* f = std::fopen(path, "rb")) {
            std::uint32_t header[3]{};
            if (std::fread(header, sizeof header, 1, f) == 1 && header[0] == k_binary_magic &&
                header[2] > 0 && header[2] < (64u << 20u)) {
                std::vector<std::uint8_t> blob(header[2]);
                if (std::fread(blob.data(), 1, blob.size(), f) == blob.size()) {
                    prog = glCreateProgram();
                    glProgramBinary(prog, header[1], blob.data(), static_cast<GLsizei>(blob.size()));
                    GLint linked = GL_FALSE;
                    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
                    if (!linked) {
                        glDeleteProgram(prog);
                        prog = 0;
                    } else {
                        ++setup_.from_cache;
                    }
                }
            }
            std::fclose(f);
            (void)glGetError(); // a rejected binary is not an error: recompile below
        }
    }

    if (!prog) {
        const GLuint v = compile(GL_VERTEX_SHADER, vs, 6);
        if (!v)
            return lx::make_error(lx::gl_err::program_failed, "ui_gl vertex shader failed to compile");
        const GLuint f = compile(GL_FRAGMENT_SHADER, fs, 6);
        if (!f) {
            glDeleteShader(v);
            return lx::make_error(lx::gl_err::program_failed, "ui_gl fragment shader failed to compile");
        }
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        if (cacheable)
            glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(prog);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint linked = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[2048];
            GLsizei len = 0;
            glGetProgramInfoLog(prog, sizeof log, &len, log);
            std::fprintf(stderr, "lx.gfx ui_gl: %s program failed to link:\n%.*s\n", src.name,
                         static_cast<int>(len), log);
            glDeleteProgram(prog);
            return lx::make_error(lx::gl_err::program_failed, "ui_gl program failed to link");
        }
        if (cacheable) {
            GLint length = 0;
            glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &length);
            if (length > 0) {
                std::vector<std::uint8_t> blob(static_cast<std::size_t>(length));
                GLenum format = 0;
                GLsizei got = 0;
                glGetProgramBinary(prog, length, &got, &format, blob.data());
                if (got > 0) {
                    // Written to a temporary and renamed: a reader never sees half a file.
                    char tmp[660];
                    std::snprintf(tmp, sizeof tmp, "%s.%d", path, static_cast<int>(::getpid()));
                    if (std::FILE* out = std::fopen(tmp, "wb")) {
                        const std::uint32_t header[3] = {k_binary_magic, format,
                                                         static_cast<std::uint32_t>(got)};
                        const bool ok = std::fwrite(header, sizeof header, 1, out) == 1 &&
                                        std::fwrite(blob.data(), 1, static_cast<std::size_t>(got),
                                                    out) == static_cast<std::size_t>(got);
                        if (std::fclose(out) == 0 && ok && std::rename(tmp, path) == 0)
                            ++setup_.written;
                        else
                            std::remove(tmp);
                    }
                }
            }
            (void)glGetError();
        }
    }

    program& p = programs_[index];
    p.id = prog;
    p.u_base = glGetUniformLocation(prog, "u_base");
    p.u_target = glGetUniformLocation(prog, "u_target");
    if (const GLint page = glGetUniformLocation(prog, "u_page"); page >= 0) {
        glUseProgram(prog);
        glUniform1i(page, 0);
        glUseProgram(0);
    }
    ++setup_.programs;
    return {};
#else
    (void)index;
    return lx::not_implemented("lx::gfx::ui_gl_renderer::build_program");
#endif
}

lx::result<void> lx::gfx::ui_gl_renderer::initialize(const ui_gl_options& options) {
#if defined(LUMEN_HAS_EGL)
    using namespace ui_gl_detail;
    shutdown();
    options_ = options;
    caps_ = {};
    setup_ = {};
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (!version || !renderer)
        return lx::make_error(lx::gl_err::no_context, "ui_gl: no current GL context");
    std::snprintf(caps_.renderer, sizeof caps_.renderer, "%s", renderer);
    std::snprintf(caps_.version, sizeof caps_.version, "%s", version);
    caps_.software = software_renderer(caps_.renderer);
    // "OpenGL ES M.m ..." — GL_MAJOR_VERSION does not exist in an ES 2 context, the string does.
    if (std::sscanf(version, "OpenGL ES %d.%d", &caps_.major, &caps_.minor) != 2)
        return lx::make_error(lx::gl_err::no_context, "ui_gl: not an OpenGL ES context");
    if (caps_.major < 3 || (caps_.major == 3 && caps_.minor < 1))
        return lx::make_error(lx::gl_err::no_context, "ui_gl: GLES 3.1 required");
    caps_.fetch = options.fetch;
    if (caps_.fetch == ui_gl_fetch::storage_buffer) {
        // GLES 3.1 lets an implementation offer no storage block in the vertex stage: then
        // the instances come in as attributes.
        GLint vs_blocks = 0;
        glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
        GLint block_size = 0;
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &block_size);
        if (vs_blocks < 1 ||
            block_size < static_cast<GLint>(k_ui_gl_ring_slots * k_ui_gl_slot_bytes))
            caps_.fetch = ui_gl_fetch::attributes;
    }

    const auto* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    caps_.buffer_storage = has_extension(ext, "GL_EXT_buffer_storage");
    caps_.disjoint_timer_query = has_extension(ext, "GL_EXT_disjoint_timer_query");
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &caps_.program_binary_formats);
    caps_.program_cache = options.program_cache && caps_.program_binary_formats > 0;

    const std::uint64_t t0 = now_ns();
    for (unsigned i = 0; i < p_count; ++i) {
        if (auto built = build_program(static_cast<program_index>(i)); !built) {
            shutdown();
            return built.get_error();
        }
    }
    setup_.program_ns = now_ns() - t0;

    // The ring.
    const auto ring_bytes = static_cast<GLsizeiptr>(k_ui_gl_ring_slots) * k_ui_gl_slot_bytes;
    glGenBuffers(1, &buffer_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer_);
    if (caps_.buffer_storage && options.persistent_ring) {
        buffer_storage_ = reinterpret_cast<void*>(eglGetProcAddress("glBufferStorageEXT"));
        if (buffer_storage_) {
            const auto storage = reinterpret_cast<PFNGLBUFFERSTORAGEEXTPROC>(buffer_storage_);
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
            storage(GL_SHADER_STORAGE_BUFFER, ring_bytes, nullptr, flags);
            if (glGetError() == GL_NO_ERROR)
                mapping_ = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, ring_bytes, flags);
            if (!mapping_) {
                // Immutable storage cannot be respecified: start over with a plain buffer.
                (void)glGetError();
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glDeleteBuffers(1, &buffer_);
                glGenBuffers(1, &buffer_);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer_);
            }
        }
    }
    caps_.persistent_ring = mapping_ != nullptr;
    if (!mapping_)
        glBufferData(GL_SHADER_STORAGE_BUFFER, ring_bytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (caps_.fetch == ui_gl_fetch::attributes) {
        // Divisors are vertex-array state: set once. Pointers are set per batch.
        glGenVertexArrays(1, &vertex_array_);
        glBindVertexArray(vertex_array_);
        for (GLuint i = 0; i < 5; ++i)
            glVertexAttribDivisor(i, 1);
        glBindVertexArray(0);
    }

    if (caps_.disjoint_timer_query && options.timer_query) {
        gen_queries_ = reinterpret_cast<void*>(eglGetProcAddress("glGenQueriesEXT"));
        delete_queries_ = reinterpret_cast<void*>(eglGetProcAddress("glDeleteQueriesEXT"));
        begin_query_ = reinterpret_cast<void*>(eglGetProcAddress("glBeginQueryEXT"));
        end_query_ = reinterpret_cast<void*>(eglGetProcAddress("glEndQueryEXT"));
        query_ui64_ = reinterpret_cast<void*>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
        if (gen_queries_ && delete_queries_ && begin_query_ && end_query_ && query_ui64_) {
            reinterpret_cast<PFNGLGENQUERIESEXTPROC>(gen_queries_)(k_ui_gl_ring_slots, queries_);
            caps_.timer_query = true;
            // A timer that never moves is no timer: time a few full clears of a small target
            // once, and fall back to the fence if that reads 0.
            GLuint rb = 0;
            GLuint fb = 0;
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 256, 256);
            glGenFramebuffers(1, &fb);
            glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(begin_query_)(GL_TIME_ELAPSED_EXT, queries_[0]);
            for (int i = 0; i < 8; ++i) {
                glClearColor(static_cast<float>(i) / 8.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            reinterpret_cast<PFNGLENDQUERYEXTPROC>(end_query_)(GL_TIME_ELAPSED_EXT);
            GLuint64 ns = 0;
            reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(query_ui64_)(queries_[0],
                                                                           GL_QUERY_RESULT_EXT, &ns);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glDeleteFramebuffers(1, &fb);
            glDeleteRenderbuffers(1, &rb);
            if (ns == 0) {
                caps_.timer_query = false;
                caps_.timer_query_reads_zero = true;
            }
        }
    }

    if (glGetError() != GL_NO_ERROR) {
        shutdown();
        return lx::make_error(lx::gl_err::program_failed, "ui_gl: GL error during initialize");
    }
    ready_ = true;
    return {};
#else
    (void)options;
    return lx::not_implemented("lx::gfx::ui_gl_renderer::initialize");
#endif
}

void lx::gfx::ui_gl_renderer::shutdown() {
#if defined(LUMEN_HAS_EGL)
    for (void*& fence : fences_) {
        if (fence)
            glDeleteSync(static_cast<GLsync>(fence));
        fence = nullptr;
    }
    if (caps_.timer_query && delete_queries_)
        reinterpret_cast<PFNGLDELETEQUERIESEXTPROC>(delete_queries_)(k_ui_gl_ring_slots, queries_);
    for (unsigned& q : queries_)
        q = 0;
    if (vertex_array_)
        glDeleteVertexArrays(1, &vertex_array_);
    if (buffer_) {
        if (mapping_) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer_);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        glDeleteBuffers(1, &buffer_);
    }
    for (program& p : programs_)
        if (p.id)
            glDeleteProgram(p.id);
    for (page_texture& t : pages_)
        if (t.name)
            glDeleteTextures(1, &t.name);
#endif
    for (program& p : programs_)
        p = {};
    for (page_texture& t : pages_)
        t = {};
    buffer_ = 0;
    vertex_array_ = 0;
    mapping_ = nullptr;
    atlas_seen_ = nullptr;
    timed_slot_ = -1;
    ready_ = false;
    caps_.timer_query = false;
}

lx::result<void> lx::gfx::ui_gl_renderer::upload_atlas(atlas& a, ui_gl_draw_stats& stats) {
#if defined(LUMEN_HAS_EGL)
    // A different atlas than last frame: every page it has goes up whole.
    const bool fresh_atlas = atlas_seen_ != &a;
    atlas_seen_ = &a;
    bool unpack_set = false;
    for (unsigned i = 0; i < k_atlas_pages; ++i) {
        const atlas_page_view view = a.page(i);
        page_texture& tex = pages_[i];
        if (!view.valid())
            continue;
        const bool r8 = view.format == atlas_format::r8;
        const unsigned bpt = r8 ? 1u : 4u;
        const GLenum format = r8 ? GL_RED : GL_RGBA;
        bool whole = fresh_atlas || a.dirty_full(i) || tex.size != view.size;
        const std::span<const lx::rect2i> dirty = a.dirty_rects(i);
        if (!whole && dirty.empty())
            continue;
        if (!unpack_set) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            unpack_set = true;
        }
        glActiveTexture(GL_TEXTURE0);
        if (tex.size != view.size) {
            // Immutable storage: a page that grew is a new texture.
            if (tex.name)
                glDeleteTextures(1, &tex.name);
            glGenTextures(1, &tex.name);
            glBindTexture(GL_TEXTURE_2D, tex.name);
            glTexStorage2D(GL_TEXTURE_2D, 1, r8 ? GL_R8 : GL_RGBA8, static_cast<GLsizei>(view.size),
                           static_cast<GLsizei>(view.size));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            tex.size = view.size;
            whole = true;
        } else {
            glBindTexture(GL_TEXTURE_2D, tex.name);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(view.stride / bpt));
        if (whole) {
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(view.size),
                            static_cast<GLsizei>(view.size), format, GL_UNSIGNED_BYTE, view.pixels);
            ++stats.atlas_uploads;
            stats.atlas_upload_bytes += static_cast<std::uint64_t>(view.size) * view.size * bpt;
        } else {
            for (const lx::rect2i& r : dirty) {
                if (r.width <= 0 || r.height <= 0 || r.x < 0 || r.y < 0 ||
                    static_cast<unsigned>(r.x + r.width) > view.size ||
                    static_cast<unsigned>(r.y + r.height) > view.size)
                    continue;
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, r.x);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, r.y);
                glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.width, r.height, format,
                                GL_UNSIGNED_BYTE, view.pixels);
                ++stats.atlas_uploads;
                stats.atlas_upload_bytes += static_cast<std::uint64_t>(r.width) * r.height * bpt;
            }
        }
        a.clear_dirty(i);
    }
    if (unpack_set) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (glGetError() != GL_NO_ERROR)
        return lx::make_error(lx::gl_err::import_failed, "ui_gl: atlas upload failed");
    return {};
#else
    (void)a;
    (void)stats;
    return lx::not_implemented("lx::gfx::ui_gl_renderer::upload_atlas");
#endif
}

namespace {

/// Copies the batch's instances (refs `first`, `count`) of one kind into `out`, in sorted
/// order, a run of consecutive indices with one memcpy. `valid(index)` rejects an instance
/// (skipped and counted). Returns the instances written.
template <typename Instance, typename Valid>
unsigned lx_gfx_ui_gl_gather(std::span<const lx::gfx::primitive_ref> refs, std::uint32_t first,
                             std::uint32_t count, std::span<const Instance> instances,
                             std::byte* out, unsigned& skipped, Valid&& valid) {
    unsigned written = 0;
    std::size_t run_start = 0;
    std::size_t run_len = 0;
    const auto flush = [&]() {
        if (run_len == 0)
            return;
        std::memcpy(out + static_cast<std::size_t>(written) * sizeof(Instance),
                    instances.data() + run_start, run_len * sizeof(Instance));
        written += static_cast<unsigned>(run_len);
        run_len = 0;
    };
    const std::size_t end = static_cast<std::size_t>(first) + count;
    for (std::size_t i = first; i < end && i < refs.size(); ++i) {
        const std::uint32_t index = refs[i].index;
        if (index >= instances.size() || !valid(instances[index])) {
            ++skipped;
            continue;
        }
        if (run_len > 0 && index == run_start + run_len) {
            ++run_len;
            continue;
        }
        flush();
        run_start = index;
        run_len = 1;
    }
    flush();
    return written;
}

[[nodiscard]] unsigned lx_gfx_ui_gl_stride(lx::gfx::primitive_kind kind) {
    using lx::gfx::primitive_kind;
    switch (kind) {
    case primitive_kind::quad: return sizeof(lx::gfx::quad_instance);
    case primitive_kind::shadow: return sizeof(lx::gfx::shadow_instance);
    case primitive_kind::mono_sprite:
    case primitive_kind::poly_sprite:
    case primitive_kind::path: return sizeof(lx::gfx::sprite_instance);
    case primitive_kind::underline: return sizeof(lx::gfx::underline_instance);
    case primitive_kind::surface: return 0;
    }
    return 0;
}

[[nodiscard]] bool lx_gfx_ui_gl_meets(lx::rect2i a, lx::rect2i b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

[[nodiscard]] lx::rect2i lx_gfx_ui_gl_intersect(lx::rect2i a, lx::rect2i b) {
    const long long x0 = a.x > b.x ? a.x : b.x;
    const long long y0 = a.y > b.y ? a.y : b.y;
    const long long ax1 = static_cast<long long>(a.x) + a.width;
    const long long bx1 = static_cast<long long>(b.x) + b.width;
    const long long ay1 = static_cast<long long>(a.y) + a.height;
    const long long by1 = static_cast<long long>(b.y) + b.height;
    const long long x1 = ax1 < bx1 ? ax1 : bx1;
    const long long y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

} // namespace

std::size_t lx::gfx::ui_gl_renderer::encode(const ui_stream& stream, const atlas* a,
                                            std::byte* slot, ui_gl_draw_stats& stats) {
    std::size_t cursor = 0;
    const std::size_t batches = stream.batches.size();
    for (std::size_t b = 0; b < batches && b < k_max_batches; ++b) {
        const batch& bt = stream.batches[b];
        batch_word_[b] = 0;
        batch_count_[b] = 0;
        const unsigned stride = lx_gfx_ui_gl_stride(bt.kind);
        if (stride == 0)
            continue; // surfaces stay blit commands
        const std::size_t need = static_cast<std::size_t>(bt.count) * stride;
        if (cursor + need > k_ui_gl_slot_bytes) {
            // warn_and_drop: the caller sees the count and warns.
            stats.overflow_drops += bt.count;
            continue;
        }
        std::byte* out = slot + cursor;
        unsigned& skipped = stats.primitives_skipped;
        unsigned n = 0;
        switch (bt.kind) {
        case primitive_kind::quad:
            n = lx_gfx_ui_gl_gather(stream.refs, bt.first, bt.count, stream.quads, out, skipped,
                                    [](const quad_instance&) { return true; });
            break;
        case primitive_kind::shadow:
            n = lx_gfx_ui_gl_gather(stream.refs, bt.first, bt.count, stream.shadows, out, skipped,
                                    [](const shadow_instance&) { return true; });
            break;
        case primitive_kind::underline:
            n = lx_gfx_ui_gl_gather(stream.refs, bt.first, bt.count, stream.underlines, out,
                                    skipped, [](const underline_instance&) { return true; });
            break;
        case primitive_kind::mono_sprite:
        case primitive_kind::poly_sprite:
        case primitive_kind::path: {
            // `ui_cpu`'s rule: the page must exist with the kind's format, the tile inside it.
            const atlas_page_view page = a ? a->page(bt.page) : atlas_page_view{};
            const atlas_format want =
                bt.kind == primitive_kind::poly_sprite ? atlas_format::rgba8 : atlas_format::r8;
            if (!page.valid() || page.format != want || bt.page >= k_atlas_pages) {
                skipped += bt.count;
                break;
            }
            const unsigned size = page.size;
            const std::uint8_t pg = bt.page;
            n = lx_gfx_ui_gl_gather(stream.refs, bt.first, bt.count, stream.sprites, out, skipped,
                                    [size, pg](const sprite_instance& sp) {
                                        return sp.page == pg && sp.uv[2] > sp.uv[0] &&
                                               sp.uv[3] > sp.uv[1] && sp.uv[2] <= size &&
                                               sp.uv[3] <= size;
                                    });
            break;
        }
        case primitive_kind::surface:
            break;
        }
        batch_word_[b] = static_cast<std::uint32_t>(cursor / 4u);
        batch_count_[b] = n;
        cursor += static_cast<std::size_t>(n) * stride;
        stats.primitives_encoded += n;
    }
    for (std::size_t b = k_max_batches; b < batches; ++b)
        stats.overflow_drops += stream.batches[b].count;
    return cursor;
}

lx::result<lx::gfx::ui_gl_draw_stats> lx::gfx::ui_gl_renderer::draw(
    unsigned framebuffer, unsigned width, unsigned height, const ui_stream& stream, atlas* atlas,
    const damage_region& damage, std::optional<lx::color> clear) {
#if defined(LUMEN_HAS_EGL)
    if (!ready_)
        return lx::not_implemented("lx::gfx::ui_gl_renderer::draw");
    if (framebuffer == 0 || width == 0 || height == 0)
        return lx::invalid_argument("ui_gl_renderer::draw: invalid framebuffer");

    ui_gl_draw_stats stats{};
    const lx::rect2i full{0, 0, static_cast<int>(width), static_cast<int>(height)};
    lx::rect2i rects[k_damage_rects]{};
    unsigned rect_count = 0;
    if (damage.full) {
        rects[rect_count++] = full;
    } else {
        for (unsigned i = 0; i < damage.count && i < k_damage_rects; ++i) {
            const lx::rect2i r = lx_gfx_ui_gl_intersect(damage.rects[i], full);
            if (r.width > 0 && r.height > 0)
                rects[rect_count++] = r;
        }
    }
    if (rect_count == 0)
        return stats;

    if (atlas) {
        if (auto up = upload_atlas(*atlas, stats); !up)
            return up.get_error();
    }

    // The slot: wait (rarely) for the frame three back that used it.
    const unsigned slot = frame_ % k_ui_gl_ring_slots;
    ++frame_;
    stats.slot = slot;
    if (fences_[slot]) {
        const std::uint64_t w0 = ui_gl_detail::now_ns();
        const GLenum waited = glClientWaitSync(static_cast<GLsync>(fences_[slot]),
                                               GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000ull);
        glDeleteSync(static_cast<GLsync>(fences_[slot]));
        fences_[slot] = nullptr;
        stats.slot_wait_ns = ui_gl_detail::now_ns() - w0;
        if (waited == GL_TIMEOUT_EXPIRED || waited == GL_WAIT_FAILED)
            return lx::make_error(lx::gl_err::draw_failed, "ui_gl: ring slot fence never signalled");
    }
    const std::size_t slot_offset = static_cast<std::size_t>(slot) * k_ui_gl_slot_bytes;

    // Encode straight into the slot.
    std::size_t bytes = 0;
    if (mapping_) {
        bytes = encode(stream, atlas, static_cast<std::byte*>(mapping_) + slot_offset, stats);
    } else {
        std::size_t upper = 0;
        for (const batch& b : stream.batches)
            upper += static_cast<std::size_t>(b.count) * lx_gfx_ui_gl_stride(b.kind);
        upper = upper < k_ui_gl_slot_bytes ? upper : k_ui_gl_slot_bytes;
        if (upper > 0) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer_);
            void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(slot_offset),
                                       static_cast<GLsizeiptr>(upper),
                                       GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                                           GL_MAP_UNSYNCHRONIZED_BIT);
            if (!p) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                return lx::make_error(lx::gl_err::draw_failed, "ui_gl: glMapBufferRange failed");
            }
            bytes = encode(stream, atlas, static_cast<std::byte*>(p), stats);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        } else {
            (void)encode(stream, atlas, nullptr, stats);
        }
    }
    stats.instance_bytes = bytes;

    const bool timed = timing_ && caps_.timer_query;
    if (timed)
        reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(begin_query_)(GL_TIME_ELAPSED_EXT, queries_[slot]);

    // Pipeline state, once for the frame.
    const auto bind_state = [&]() {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glDisable(GL_DITHER);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        glEnable(GL_SCISSOR_TEST);
        // The whole ring, the same binding every frame; the slot is in `u_base`. (Rebinding
        // per slot with glBindBufferRange put stale instances on screen on SVGA3D: the
        // offset-0 slot after the 4 MiB one kept reading the old offset.)
        if (bytes > 0) {
            if (vertex_array_) {
                glBindVertexArray(vertex_array_);
                glBindBuffer(GL_ARRAY_BUFFER, buffer_);
            } else {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer_);
            }
        }
        glActiveTexture(GL_TEXTURE0);
    };
    unsigned enabled_arrays = 0;
    bind_state();

    int bound_program = -1;
    int bound_page = -1;
    const auto use = [&](unsigned index) {
        if (bound_program == static_cast<int>(index))
            return;
        bound_program = static_cast<int>(index);
        const program& p = programs_[index];
        glUseProgram(p.id);
        if (p.u_target >= 0)
            glUniform2f(p.u_target, static_cast<float>(width), static_cast<float>(height));
    };

    const std::size_t batch_total = stream.batches.size() < k_max_batches ? stream.batches.size()
                                                                           : k_max_batches;
    for (unsigned n = 0; n < rect_count; ++n) {
        const lx::rect2i rect = rects[n];
        ++stats.rects;
        glScissor(rect.x, rect.y, rect.width, rect.height);
        if (clear) {
            glDisable(GL_BLEND);
            glClearColor(clear->r, clear->g, clear->b, clear->a);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glEnable(GL_BLEND);
        for (std::size_t b = 0; b < batch_total; ++b) {
            const batch& bt = stream.batches[b];
            if (!lx_gfx_ui_gl_meets(bt.bounds, rect)) {
                ++stats.batches_skipped;
                continue;
            }
            ++stats.batches_drawn;
            if (bt.kind == primitive_kind::surface) {
                const std::size_t end = static_cast<std::size_t>(bt.first) + bt.count;
                for (std::size_t i = bt.first; i < end && i < stream.refs.size(); ++i) {
                    const std::uint32_t index = stream.refs[i].index;
                    if (!compositor_ || index >= stream.surfaces.size()) {
                        ++stats.primitives_skipped;
                        continue;
                    }
                    damage_region one{};
                    one.rects[0] = rect;
                    one.count = 1;
                    // The compositor's own path; it leaves its state reset, so ours is set again.
                    glUseProgram(0);
                    if (vertex_array_) {
                        glBindVertexArray(0);
                        glBindBuffer(GL_ARRAY_BUFFER, 0);
                    }
                    auto drawn = compositor_->composite_into(framebuffer, width, height, {},
                                                             &stream.surfaces[index], 1, one,
                                                             false, false);
                    bind_state();
                    glScissor(rect.x, rect.y, rect.width, rect.height);
                    glEnable(GL_BLEND);
                    bound_program = -1;
                    bound_page = -1;
                    if (!drawn)
                        return drawn.get_error();
                    ++stats.draw_calls;
                }
                continue;
            }
            const std::uint32_t count = batch_count_[b];
            if (count == 0)
                continue;
            unsigned index = p_quad;
            switch (bt.kind) {
            case primitive_kind::shadow: index = p_shadow; break;
            case primitive_kind::quad: index = p_quad; break;
            case primitive_kind::mono_sprite:
            case primitive_kind::path: index = p_mono; break;
            case primitive_kind::poly_sprite: index = p_poly; break;
            case primitive_kind::underline: index = p_underline; break;
            case primitive_kind::surface: break;
            }
            use(index);
            if (index == p_mono || index == p_poly) {
                if (bound_page != bt.page) {
                    bound_page = bt.page;
                    glBindTexture(GL_TEXTURE_2D, pages_[bt.page].name);
                }
            }
            if (vertex_array_) {
                // GLES has no base instance: the batch's offset goes in the pointers.
                const unsigned vec4s = ui_gl_detail::k_programs[index].vec4s;
                const auto stride = static_cast<GLsizei>(vec4s * 16u);
                const std::size_t base = slot_offset + static_cast<std::size_t>(batch_word_[b]) * 4u;
                for (unsigned k = 0; k < vec4s; ++k)
                    glVertexAttribIPointer(k, 4, GL_UNSIGNED_INT, stride,
                                           reinterpret_cast<const void*>(base + 16u * k));
                for (; enabled_arrays < vec4s; ++enabled_arrays)
                    glEnableVertexAttribArray(enabled_arrays);
                for (; enabled_arrays > vec4s; --enabled_arrays)
                    glDisableVertexAttribArray(enabled_arrays - 1u);
            } else {
                glUniform1ui(programs_[index].u_base,
                             static_cast<GLuint>(slot_offset / 4u) + batch_word_[b]);
            }
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(count));
            ++stats.draw_calls;
        }
    }

    if (timed)
        reinterpret_cast<PFNGLENDQUERYEXTPROC>(end_query_)(GL_TIME_ELAPSED_EXT);

    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (vertex_array_) {
        for (; enabled_arrays > 0; --enabled_arrays)
            glDisableVertexAttribArray(enabled_arrays - 1u);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    fences_[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (timing_) {
        timed_slot_ = static_cast<int>(slot);
        timed_flush_ns_ = ui_gl_detail::now_ns();
    }
    if (glGetError() != GL_NO_ERROR)
        return lx::make_error(lx::gl_err::draw_failed, "ui_gl: GL error during draw");
    return stats;
#else
    (void)framebuffer;
    (void)width;
    (void)height;
    (void)stream;
    (void)atlas;
    (void)damage;
    (void)clear;
    return lx::not_implemented("lx::gfx::ui_gl_renderer::draw");
#endif
}

lx::result<lx::gfx::ui_gl_gpu_time> lx::gfx::ui_gl_renderer::wait_gpu_time() {
#if defined(LUMEN_HAS_EGL)
    ui_gl_gpu_time out{};
    if (!ready_ || timed_slot_ < 0)
        return out;
    const auto slot = static_cast<unsigned>(timed_slot_);
    timed_slot_ = -1;
    if (caps_.timer_query) {
        GLuint64 ns = 0;
        reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(query_ui64_)(queries_[slot],
                                                                       GL_QUERY_RESULT_EXT, &ns);
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        out.source = ui_gl_timing::timer_query;
        out.ns = ns;
        out.disjoint = disjoint != 0;
    } else {
        if (fences_[slot])
            (void)glClientWaitSync(static_cast<GLsync>(fences_[slot]), GL_SYNC_FLUSH_COMMANDS_BIT,
                                   1'000'000'000ull);
        out.source = ui_gl_timing::fence_wall;
        out.ns = ui_gl_detail::now_ns() - timed_flush_ns_;
    }
    if (glGetError() != GL_NO_ERROR)
        return lx::make_error(lx::gl_err::draw_failed, "ui_gl: GPU time query failed");
    return out;
#else
    return lx::not_implemented("lx::gfx::ui_gl_renderer::wait_gpu_time");
#endif
}
