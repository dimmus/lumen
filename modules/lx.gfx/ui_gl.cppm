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
import lx.runtime;

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
// (shaders/ui/*.glsl, the same files `:ui_vk` compiles to SPIR-V) transcribe its functions
// (the same SDF, border, dash, gradient, the same four-sample erf shadow, the same Taylor
// sine for wavy underlines and gradient directions).
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
    /// What `gl_compositor::composite_into` reported for the surface runs, summed over
    /// (run, damage rect) pairs: draws of an unregistered texture, draws clipped away.
    unsigned surface_draws_skipped = 0;
    unsigned surface_draws_culled = 0;
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

    /// The affinity `draw` and `wait_gpu_time` must run on (ADR-010 §4.12): `render` in the
    /// compositor, `ui` in a client. `any` (the default) checks nothing — the tests, the bench
    /// and the compositor's start-up warm draw, before the context moves to the render thread.
    void set_owner_affinity(lx::runtime::affinity owner) { owner_ = owner; }

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
    lx::runtime::affinity owner_ = lx::runtime::affinity::any;
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

// The GLSL is shaders/ui/*.glsl, shared with lx.gfx:ui_vk (which compiles the same files to
// SPIR-V at build time); lumen_embed_glsl turns each into a raw string literal. What differs
// between the backends is the prologue below: how an instance word is fetched, where the
// target size comes from, the vertex index, the fragment output and the atlas page sampler.
//
// Deterministic helpers shared by both stages: `ui_math::sin` (the CPU's Taylor series after
// the same range reduction), the sRGB curves and Oklab of `ui_raster::to_oklab`/`from_oklab`.
constexpr const char* k_math =
#include "shaders/ui/math.glsl.h"
    ;

// The vertex prologue: instance fetch. Words come from per-instance integer attributes (four
// per attribute, the batch's offset in the attribute pointers) or from the ring as a storage
// buffer (`u_base` + instance · stride). `i` is a constant at every call site, so the
// attribute selection folds away.
constexpr const char* k_vertex_gl = R"(
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
#define LX_VERTEX_ID gl_VertexID
)";

// The vertex stage proper: the raster window and the whole-pixel quad.
constexpr const char* k_vertex_common =
#include "shaders/ui/vertex.glsl.h"
    ;

constexpr const char* k_fragment_gl = R"(
layout(location = 0) out vec4 o_color;
)";

constexpr const char* k_fragment_common =
#include "shaders/ui/fragment.glsl.h"
    ;

// ── quad ──

constexpr const char* k_quad_vs =
#include "shaders/ui/quad.vs.glsl.h"
    ;
constexpr const char* k_quad_fs =
#include "shaders/ui/quad.fs.glsl.h"
    ;

// ── shadow ──

constexpr const char* k_shadow_vs =
#include "shaders/ui/shadow.vs.glsl.h"
    ;
constexpr const char* k_shadow_fs =
#include "shaders/ui/shadow.fs.glsl.h"
    ;

// ── sprites (mono and path: R8; poly with LX_POLY: RGBA8) ──

constexpr const char* k_sprite_vs =
#include "shaders/ui/sprite.vs.glsl.h"
    ;
// The page is bound to texture unit 0 per batch.
constexpr const char* k_sprite_fs_gl = R"(
uniform highp sampler2D u_page;
vec4 lx_page_texel(ivec2 p) { return texelFetch(u_page, p, 0); }
)";
constexpr const char* k_sprite_fs =
#include "shaders/ui/sprite.fs.glsl.h"
    ;

// ── underline ──

constexpr const char* k_underline_vs =
#include "shaders/ui/underline.vs.glsl.h"
    ;
constexpr const char* k_underline_fs =
#include "shaders/ui/underline.fs.glsl.h"
    ;

struct program_source {
    const char* name;
    const char* defines; ///< after the header, both stages
    const char* vs;
    const char* fs_prologue; ///< GLES-only fragment declarations before `fs`
    const char* fs;
    unsigned vec4s; ///< instance size in 16-byte attributes
};

/// By `program_index`.
constexpr program_source k_programs[] = {
    {"shadow", "#define LX_STRIDE 16u\n#define LX_VEC4S 4\n", k_shadow_vs, "\n", k_shadow_fs, 4},
    {"quad", "#define LX_STRIDE 20u\n#define LX_VEC4S 5\n", k_quad_vs, "\n", k_quad_fs, 5},
    {"mono_sprite", "#define LX_STRIDE 16u\n#define LX_VEC4S 4\n", k_sprite_vs, k_sprite_fs_gl,
     k_sprite_fs, 4},
    {"poly_sprite", "#define LX_STRIDE 16u\n#define LX_VEC4S 4\n#define LX_POLY 1\n", k_sprite_vs,
     k_sprite_fs_gl, k_sprite_fs, 4},
    {"underline", "#define LX_STRIDE 12u\n#define LX_VEC4S 3\n", k_underline_vs, "\n",
     k_underline_fs, 3},
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
    const char* vs[] = {k_header, fetch, src.defines, k_math, k_vertex_gl, k_vertex_common, src.vs};
    const char* fs[] = {k_header,      fetch,             src.defines,     k_math,
                        k_fragment_gl, k_fragment_common, src.fs_prologue, src.fs};

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
        const GLuint v = compile(GL_VERTEX_SHADER, vs, static_cast<GLsizei>(std::size(vs)));
        if (!v)
            return lx::make_error(lx::gl_err::program_failed, "ui_gl vertex shader failed to compile");
        const GLuint f = compile(GL_FRAGMENT_SHADER, fs, static_cast<GLsizei>(std::size(fs)));
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

std::size_t lx::gfx::ui_gl_renderer::encode(const ui_stream& stream, const atlas* a,
                                            std::byte* slot, ui_gl_draw_stats& stats) {
    ui_encode::encode_counts counts{};
    const std::size_t bytes = ui_encode::encode(stream, a, slot, k_ui_gl_slot_bytes, batch_word_,
                                                batch_count_, counts);
    stats.primitives_encoded += counts.encoded;
    stats.primitives_skipped += counts.skipped;
    stats.overflow_drops += counts.overflow_drops;
    return bytes;
}

lx::result<lx::gfx::ui_gl_draw_stats> lx::gfx::ui_gl_renderer::draw(
    unsigned framebuffer, unsigned width, unsigned height, const ui_stream& stream, atlas* atlas,
    const damage_region& damage, std::optional<lx::color> clear) {
#if defined(LUMEN_HAS_EGL)
    lx::runtime::assert_affinity(owner_);
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
            const lx::rect2i r = ui_encode::intersect(damage.rects[i], full);
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
            upper += static_cast<std::size_t>(b.count) * ui_encode::stride(b.kind);
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
            if (!ui_encode::meets(bt.bounds, rect)) {
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
                    // Refs naming consecutive draws (the compositor's windows, in paint
                    // order) go through one call: the same pixels, one state setup and one
                    // flush for the run.
                    unsigned run = 1;
                    while (i + run < end && i + run < stream.refs.size() &&
                           stream.refs[i + run].index == index + run &&
                           index + run < stream.surfaces.size())
                        ++run;
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
                                                             &stream.surfaces[index], run, one,
                                                             false, false, false);
                    i += run - 1;
                    bind_state();
                    glScissor(rect.x, rect.y, rect.width, rect.height);
                    glEnable(GL_BLEND);
                    bound_program = -1;
                    bound_page = -1;
                    if (!drawn)
                        return drawn.get_error();
                    ++stats.draw_calls;
                    stats.surface_draws_skipped += drawn->draws_skipped;
                    stats.surface_draws_culled += drawn->draws_culled;
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
    lx::runtime::assert_affinity(owner_);
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
