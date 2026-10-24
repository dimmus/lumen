module;

#if defined(LUMEN_HAS_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
// GLES 3 headers (a superset of 2): `copy_region` blits when the context is ES 3.0.
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <unistd.h>
#endif
#include <stdint.h>

import lx.foundation;

export module lx.gfx:gl_renderer;

import std;

import :renderer;
import :dmabuf;
import :damage;

export namespace lx::gfx {

/// Description of a scanout buffer the GL backend rendered into, handed to KMS.
struct gl_scanout_dmabuf {
    lx::unique_fd fd{};
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride = 0;
    unsigned offset = 0;
    unsigned long long modifier = 0;
    lx::fourcc format = 0;
};

/// EGL/GBM device: the GL counterpart of `gfx::device`.
///
/// Deliberately separate from `gfx::device` rather than folded into it. The Vulkan device
/// owns a `VkDevice` and its queues; this owns a `gbm_device`, an `EGLDisplay` and a
/// context bound to one thread. They have different lifetimes and different thread rules,
/// and a compositor run uses exactly one of them for present.
class egl_device {
public:
    egl_device() = default;
    ~egl_device();

    egl_device(const egl_device&) = delete;
    egl_device& operator=(const egl_device&) = delete;
    egl_device(egl_device&& other) noexcept;
    egl_device& operator=(egl_device&& other) noexcept;

    /// `drm_fd` is borrowed — a card node with DRM master, or a render node. The caller
    /// keeps ownership and must outlive this device.
    [[nodiscard]] static lx::result<egl_device> create(int drm_fd);

    /// Binds the context to the calling thread. The GL backend is single-threaded by
    /// construction: everything after this must run on the render affinity.
    [[nodiscard]] lx::result<void> make_current();

    /// Unbinds the context from the calling thread. An EGL context is current on at most
    /// one thread at a time, so setup on the UI thread must release it before the render
    /// thread can claim it.
    void release_current();

    [[nodiscard]] bool valid() const { return context_ != nullptr; }

    /// `GL_RENDERER`, e.g. "SVGA3D; build: RELEASE;  LLVM;". Empty until `make_current`.
    [[nodiscard]] const char* renderer() const { return renderer_; }

    /// True when GL is running on a software rasterizer (llvmpipe, softpipe, swrast),
    /// where the GL path has no advantage over compositing on the CPU directly.
    [[nodiscard]] bool is_software_renderer() const { return software_; }

    /// EGL_EXT_image_dma_buf_import — required to sample client dma-bufs without a copy.
    [[nodiscard]] bool supports_dmabuf_import() const { return has_dmabuf_import_; }

    /// EGL_ANDROID_native_fence_sync — lets a frame's completion be exported as a
    /// sync_file the atomic commit waits on, instead of the render thread blocking on it.
    [[nodiscard]] bool supports_native_fence() const { return has_native_fence_; }

    /// The context's GLES version (`GL_VERSION`), 0.0 until `make_current`. `create` asks
    /// for 3.1 and falls back to 2.0; `at_least(3, 1)` is what `:ui_gl` needs.
    [[nodiscard]] int gles_major() const { return gles_major_; }
    [[nodiscard]] int gles_minor() const { return gles_minor_; }
    [[nodiscard]] bool gles_at_least(int major, int minor) const {
        return gles_major_ > major || (gles_major_ == major && gles_minor_ >= minor);
    }

    [[nodiscard]] void* egl_display() const { return display_; }
    [[nodiscard]] void* gbm_device_handle() const { return gbm_; }

private:
    void destroy();

    void* gbm_ = nullptr;
    void* display_ = nullptr;
    void* context_ = nullptr;
    char renderer_[128]{};
    int gles_major_ = 0;
    int gles_minor_ = 0;
    bool software_ = false;
    bool has_dmabuf_import_ = false;
    bool has_native_fence_ = false;
    bool owns_gbm_ = false;
};

/// A GBM buffer object bound as a GL framebuffer and exportable for scanout.
///
/// This is the "render into something KMS can flip" primitive: `gbm_bo` with
/// SCANOUT|RENDERING, wrapped as an `EGLImage`, attached to an FBO as a renderbuffer.
class gl_scanout_target {
public:
    gl_scanout_target() = default;
    ~gl_scanout_target();

    gl_scanout_target(const gl_scanout_target&) = delete;
    gl_scanout_target& operator=(const gl_scanout_target&) = delete;
    gl_scanout_target(gl_scanout_target&& other) noexcept;
    gl_scanout_target& operator=(gl_scanout_target&& other) noexcept;

    [[nodiscard]] static lx::result<gl_scanout_target> create(const egl_device& device,
                                                              unsigned width, unsigned height,
                                                              lx::fourcc format);

    [[nodiscard]] bool valid() const { return framebuffer_ != 0; }
    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }
    /// GL framebuffer name to bind before drawing.
    [[nodiscard]] unsigned framebuffer() const { return framebuffer_; }

    /// Exports the underlying BO for `drm::kms_framebuffer::import_dmabuf`.
    [[nodiscard]] lx::result<gl_scanout_dmabuf> export_dmabuf() const;

private:
    void destroy();

    void* bo_ = nullptr;
    void* image_ = nullptr;
    void* display_ = nullptr;
    unsigned renderbuffer_ = 0;
    unsigned framebuffer_ = 0;
    unsigned width_ = 0;
    unsigned height_ = 0;
    lx::fourcc format_ = 0;
};

struct gl_composite_stats {
    unsigned draws_submitted = 0;
    unsigned draws_skipped = 0;
    /// Clipped away entirely, so never submitted. Expected, not an error.
    unsigned draws_culled = 0;
    unsigned uploads = 0;
    unsigned dmabuf_imports = 0;
    /// sync_file signalled when this frame's GL work completes, or -1 when the driver has
    /// no native fence support — in which case `composite` already blocked instead.
    ///
    /// **Owned by the caller**, which must either hand it to an atomic commit as
    /// `in_fence_fd` or close it. Leaking it leaks a file descriptor per frame.
    int out_fence_fd = -1;
};

/// Hardware composite through GLES 2. Peer of `vulkan_compositor` and `cpu_compositor`.
///
/// Client buffers arrive one of two ways, and the difference dominates the frame cost:
/// a dma-buf is imported as an `EGLImage` and sampled in place (no copy), while shm pixels
/// must be uploaded. Uploading into a texture the previous frame is still sampling stalls
/// the pipeline badly, so each texture keeps two GL names and alternates between them.
class gl_compositor {
public:
    static constexpr unsigned k_max_textures = 256;

    gl_compositor() = default;
    ~gl_compositor();

    gl_compositor(const gl_compositor&) = delete;
    gl_compositor& operator=(const gl_compositor&) = delete;

    /// Requires `device.make_current()` on this thread first.
    [[nodiscard]] lx::result<void> initialize(egl_device& device);
    void shutdown();
    [[nodiscard]] bool ready() const { return programs_[0].id != 0 && programs_[1].id != 0; }

    /// Uploads CPU pixels in RGBA order. `stride` is bytes per row; 0 means packed.
    [[nodiscard]] lx::result<void> upload_rgba(unsigned texture_id, unsigned width,
                                               unsigned height, const unsigned char* rgba,
                                               unsigned stride = 0);
    /// Updates `region` of the texture's `width` x `height` storage from packed rows
    /// (`region.width * 4` bytes each). The texture name being written must already hold
    /// the rest of the image — the caller stages the union of what changed since that name
    /// was last written (buffer age 2, see the two names per slot). A name with no storage
    /// of this size is allocated and only the region filled, and counted in
    /// `partial_into_fresh()`: it means the caller's bookkeeping and the slot disagree.
    [[nodiscard]] lx::result<void> upload_rgba_region(unsigned texture_id, unsigned width,
                                                      unsigned height, lx::rect2i region,
                                                      const unsigned char* rgba);

    /// Uploads `region` of a client's shm buffer straight from its memory: `pixels` is the
    /// region's first pixel, rows `stride` bytes apart, in wl_shm ARGB8888/XRGB8888 order
    /// (BGRA bytes). An empty region is the whole `width` x `height` image.
    /// No staging copy and no swizzle on the CPU — the driver reads the client's rows
    /// once. `opaque` (XRGB) makes the sampler treat alpha as one. Needs
    /// `supports_client_upload`.
    [[nodiscard]] lx::result<void> upload_client_region(unsigned texture_id, unsigned width,
                                                        unsigned height, lx::rect2i region,
                                                        const unsigned char* pixels,
                                                        unsigned stride, bool opaque);
    /// The same for several rects of one update, in one alternation of the texture's
    /// names: `pixels` is `region`'s first pixel and every rect of `parts` lies inside
    /// `region` (its bounds). A terminal that changed its cursor line and its status line
    /// uploads two strips, not the window between them.
    [[nodiscard]] lx::result<void> upload_client_regions(unsigned texture_id, unsigned width,
                                                         unsigned height, lx::rect2i region,
                                                         std::span<const lx::rect2i> parts,
                                                         const unsigned char* pixels,
                                                         unsigned stride, bool opaque);
    /// GL_EXT_texture_format_BGRA8888 and GL_EXT_unpack_subimage: client rows can be
    /// uploaded as they are, at their own stride.
    [[nodiscard]] bool supports_client_upload() const { return bgra_ && unpack_subimage_; }

    /// Samples a client dma-buf in place via EGL_EXT_image_dma_buf_import.
    [[nodiscard]] lx::result<void> import_dmabuf(unsigned texture_id, const dmabuf_desc& desc);

    void forget_texture(unsigned texture_id);
    /// Partial uploads that landed in a name without prior content (should stay 0).
    [[nodiscard]] unsigned partial_into_fresh() const { return partial_into_fresh_; }

    [[nodiscard]] lx::result<gl_composite_stats> composite(gl_scanout_target& target,
                                                           lx::color clear,
                                                           const blit_command* cmds,
                                                           unsigned count, lx::rect2i damage);

    /// Composites only the rects of `damage`: pipeline state is set once, then each rect
    /// is scissored, cleared and drawn, with draws that miss the rect skipped outright.
    /// One fence covers the whole frame. Full means the whole target; empty draws nothing
    /// but still returns a valid (fence-less) result.
    [[nodiscard]] lx::result<gl_composite_stats> composite_region(gl_scanout_target& target,
                                                                  lx::color clear,
                                                                  const blit_command* cmds,
                                                                  unsigned count,
                                                                  const damage_region& damage);

    /// `composite_region` into any framebuffer object (`width` x `height`, row 0 on top as
    /// in a scanout target): a screen-capture target over a client's dma-buf, an offscreen
    /// buffer. `clear_first` false draws over what is there (a pointer painted onto a copy);
    /// `fence` false only flushes — the caller fences the whole capture once (and never
    /// blocks: no glFinish either way). `flush` false (with `fence` false) does not even
    /// flush: a caller drawing more into the same frame (the UI renderer's surface runs)
    /// submits it once at its end.
    [[nodiscard]] lx::result<gl_composite_stats> composite_into(unsigned framebuffer,
                                                                unsigned width, unsigned height,
                                                                lx::color clear,
                                                                const blit_command* cmds,
                                                                unsigned count,
                                                                const damage_region& damage,
                                                                bool clear_first, bool fence,
                                                                bool flush = true);

    /// Closes a frame other code drew into a scanout target on this context (the UI
    /// primitive renderer, `:ui_gl`, drawing a frame's stream): exports its completion as a
    /// sync_file — the `out_fence_fd` `composite` returns, owned by the caller — or, with no
    /// native fence support, blocks until it is done and returns -1.
    [[nodiscard]] lx::result<int> fence_frame();

    /// Copies `region` (full: everything) of `src` into the same place of `dst`, a target of
    /// the same size: a scanout slot brought up to date from the one on screen while the
    /// output is idle, so its next composite draws only that frame's damage. Needs
    /// `supports_target_copy` (OpenGL ES 3.0, glBlitFramebuffer). Flushed, not fenced: a
    /// later composite into `dst` is ordered after it in this context, and its fence covers it.
    [[nodiscard]] lx::result<void> copy_region(const gl_scanout_target& src,
                                               gl_scanout_target& dst,
                                               const damage_region& region);
    [[nodiscard]] bool supports_target_copy() const { return target_copy_; }

    /// Reads the target back as RGBA rows, top row first. For tests and screenshots — it
    /// stalls the pipeline and is not part of the frame path.
    [[nodiscard]] lx::result<void> read_back(const gl_scanout_target& target, unsigned char* rgba,
                                             unsigned capacity);

private:
    struct texture_slot {
        unsigned id = 0;
        /// Two names so an upload never targets the texture the previous frame is
        /// sampling. Unused for dma-buf slots, which are never written by the CPU.
        unsigned names[2]{};
        /// Storage size of each name. The two alternate, so after a resize the name about
        /// to be written may still hold the size before last — `width`/`height` (the size
        /// being sampled) cannot tell whether it needs reallocating.
        unsigned name_width[2]{};
        unsigned name_height[2]{};
        /// GL format each name's storage was allocated with (GL_RGBA or GL_BGRA_EXT): a
        /// sub-image upload must match it. Sampled alpha is forced to one where opaque.
        unsigned name_format[2]{};
        bool name_opaque[2]{};
        unsigned current = 0;
        unsigned width = 0;
        unsigned height = 0;
        void* image = nullptr;
        bool is_dmabuf = false;
        bool used = false;
    };

    [[nodiscard]] int find(unsigned texture_id) const;
    [[nodiscard]] int alloc(unsigned texture_id);
    void destroy_slot(texture_slot& slot);
    /// Shared body: `rect_count == 0` is the whole target. `clear_first`: each rect is
    /// cleared before its draws; `fence`: a native fence (or glFinish without one) closes
    /// the work, otherwise only a flush.
    [[nodiscard]] lx::result<gl_composite_stats> composite_rects(unsigned framebuffer,
                                                                 unsigned width_px,
                                                                 unsigned height_px,
                                                                 lx::color clear,
                                                                 const blit_command* cmds,
                                                                 unsigned count,
                                                                 const lx::rect2i* rects,
                                                                 unsigned rect_count,
                                                                 bool clear_first = true,
                                                                 bool fence = true,
                                                                 bool flush = true);

    /// One composite program and its uniform locations (-1: optimised out).
    struct composite_program {
        unsigned id = 0;
        int rect = -1;
        int target = -1;
        int opacity = -1;
        int uv_origin_dx = -1;
        int uv_dy = -1;
        int tint = -1;
        int src_bounds = -1;
        int transfer = -1;
        int alpha_one = -1;
        int use_solid = -1;
        int solid_texel = -1;
        int sampler = -1;
    };
    /// [0] decodes to linear light, tints and re-encodes; [1] is the identity variant for
    /// an sRGB source without a colour tint, the common case (LUMEN_IDENTITY in the shader).
    static constexpr unsigned k_program_linear = 0;
    static constexpr unsigned k_program_identity = 1;

    egl_device* device_ = nullptr;
    composite_program programs_[2]{};
    unsigned partial_into_fresh_ = 0;
    unsigned vertex_buffer_ = 0;
    bool bgra_ = false;
    bool unpack_subimage_ = false;
    bool target_copy_ = false;
    texture_slot textures_[k_max_textures]{};
};

} // namespace lx::gfx


namespace lx::gfx::detail {

#if defined(LUMEN_HAS_EGL)

/// Ends a frame's GL work: a native fence's sync_file when the device exports one (the
/// caller owns it), else `glFinish` and -1.
[[nodiscard]] int gl_frame_fence(egl_device* device);

/// GLES 2 is the floor every driver supports, and a compositor's shader needs nothing
/// beyond it: place a quad from a rect, sample one texture, scale by opacity.
constexpr const char* k_vertex_shader = R"(#version 100
attribute vec2 corner;
uniform vec4 rect;
uniform vec2 target;
// The source box as an affine basis in texture coordinates: where the destination's
// top-left lands (xy) and where its +x (zw) and +y (uv_dy) directions go. Cropping,
// fractional scale and the eight buffer transforms are then the same draw with different
// vectors rather than separate paths (lx::gfx::source_basis_for).
uniform vec4 uv_origin_dx;
uniform vec2 uv_dy;
// highp: the vertex stage is highp by definition, and the fragment side declares the same
// varying highp where it can. mediump (fp16 on some hardware) cannot address texels past
// about 2048, which shows as smeared columns on a wide buffer.
varying highp vec2 uv;
void main() {
    uv = uv_origin_dx.xy + corner.x * uv_origin_dx.zw + corner.y * uv_dy;
    vec2 px = rect.xy + corner * rect.zw;
    // Top-left pixel space straight to NDC, unflipped. The target is a framebuffer object
    // over a GBM buffer, and in a framebuffer object window y = 0 is row 0 of the buffer's
    // memory — the line KMS scans out first, the top of the screen. GL's bottom-left origin
    // only matters for a default (window-system) framebuffer, which this never is.
    // Negating y here, as this once did, put every frame on screen upside down; the
    // read-back below reversed rows too, so the tests agreed with each other and not with
    // the display. test_gl_composite now checks the scanout memory itself.
    vec2 ndc = (px / target) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

// Compiled twice: as is, and with LUMEN_IDENTITY defined (see gl_compositor::initialize).
constexpr const char* k_fragment_shader = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define UV_PRECISION highp
#else
#define UV_PRECISION mediump
#endif
precision mediump float;
varying UV_PRECISION vec2 uv;
uniform sampler2D surface;
uniform float opacity;
uniform vec4 tint;
uniform UV_PRECISION vec4 src_bounds;
// 0 linear, 1 sRGB, 2 gamma 2.2. GLES 2 has no HDR path, so PQ and HLG fall back to sRGB
// rather than pretending — the Vulkan backend is where HDR lands.
uniform int transfer;
// 1.0 for a format without alpha (XRGB): its fourth byte is undefined, not transparency.
uniform float alpha_one;
// A solid draw (wp_single_pixel_buffer_v1): `solid_texel` replaces the sample — the same
// premultiplied, encoded value a 1x1 texture would hold, so it is decoded, tinted and faded
// exactly like one, without a texture or an upload.
uniform float use_solid;
uniform vec4 solid_texel;

vec3 srgb_to_linear(vec3 v) {
    // GLSL ES 1.00 has no mix(genType, genType, bvec), so branch per channel arithmetically.
    vec3 lo = v / 12.92;
    vec3 hi = pow((max(v, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    vec3 pick = step(vec3(0.04045), v);
    return lo * (1.0 - pick) + hi * pick;
}

vec3 linear_to_srgb(vec3 v) {
    vec3 lo = v * 12.92;
    vec3 hi = 1.055 * pow(max(v, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    vec3 pick = step(vec3(0.0031308), v);
    return lo * (1.0 - pick) + hi * pick;
}

void main() {
    // Clamp to the source rectangle. Linear filtering samples between texel centers, so a
    // magnified crop would otherwise read texels from outside its own source rect.
    UV_PRECISION vec2 s = clamp(uv, src_bounds.xy, src_bounds.zw);
    vec4 texel = use_solid > 0.5 ? solid_texel : texture2D(surface, s);
    texel.a = max(texel.a, alpha_one);
#ifdef LUMEN_IDENTITY
    // An sRGB source without a colour tint: decoding and re-encoding is the identity, so
    // opacity and tint alpha only scale the premultiplied texel. This skips the six pow()
    // below, most of the shader's cost: a fullscreen draw on SVGA3D went from 5.5 to 3 ms.
    // A separate program rather than a branch on a uniform, which Mesa specialises the
    // shader on per draw (it cost 16 µs of submission per draw).
    gl_FragColor = texel * (opacity * tint.a);
#else

    // Blending is a weighted average of light, so it has to happen in linear space.
    // GLES 2 has no sRGB framebuffer to do this in the blend unit and no float attachment
    // to blend in, so the shader decodes, applies opacity and tint in linear, and re-encodes
    // before the blend. That is correct for the draw's own color; the blend against what is
    // already in the framebuffer still happens in encoded space, which is the limit of this
    // backend. Vulkan blends linear end to end via an sRGB attachment.
    vec3 straight = texel.a > 0.0 ? texel.rgb / texel.a : texel.rgb;
    vec3 lin = transfer == 0 ? straight
             : (transfer == 2 ? pow(max(straight, vec3(0.0)), vec3(2.2))
                              : srgb_to_linear(straight));

    float alpha = texel.a * opacity * tint.a;
    vec3 shaded = linear_to_srgb(lin * tint.rgb);
    gl_FragColor = vec4(shaded * alpha, alpha);
#endif
}
)";

/// `prefix` goes before `source` (after the #version line, which must come first).
unsigned compile_shader(unsigned kind, const char* source, const char* prefix = nullptr) {
    const GLuint shader = glCreateShader(kind);
    if (!shader)
        return 0;
    if (prefix) {
        const char* parts[3] = {"#version 100\n", prefix, source};
        glShaderSource(shader, 3, parts, nullptr);
    } else {
        glShaderSource(shader, 1, &source, nullptr);
    }
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool egl_has_extension(const char* extensions, const char* name) {
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

/// Mesa reports software rasterizers under a handful of names; none of them are worth
/// routing a frame through the GL stack for.
bool renderer_is_software(const char* renderer) {
    if (!renderer)
        return true;
    static const char* const names[] = {"llvmpipe", "softpipe", "swrast", "SWR", "Software"};
    for (const char* name : names) {
        if (std::strstr(renderer, name))
            return true;
    }
    return false;
}

#endif // LUMEN_HAS_EGL

} // namespace lx::gfx::detail

// ── egl_device ───────────────────────────────────────────────────────────────

lx::gfx::egl_device::~egl_device() { destroy(); }

lx::gfx::egl_device::egl_device(egl_device&& other) noexcept {
    *this = std::move(other);
}

lx::gfx::egl_device& lx::gfx::egl_device::operator=(egl_device&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    gbm_ = other.gbm_;
    display_ = other.display_;
    context_ = other.context_;
    software_ = other.software_;
    has_dmabuf_import_ = other.has_dmabuf_import_;
    has_native_fence_ = other.has_native_fence_;
    owns_gbm_ = other.owns_gbm_;
    std::memcpy(renderer_, other.renderer_, sizeof(renderer_));
    gles_major_ = other.gles_major_;
    gles_minor_ = other.gles_minor_;
    other.gbm_ = nullptr;
    other.display_ = nullptr;
    other.context_ = nullptr;
    other.owns_gbm_ = false;
    other.renderer_[0] = '\0';
    return *this;
}

void lx::gfx::egl_device::destroy() {
#if defined(LUMEN_HAS_EGL)
    if (display_) {
        auto dpy = static_cast<EGLDisplay>(display_);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_)
            eglDestroyContext(dpy, static_cast<EGLContext>(context_));
        eglTerminate(dpy);
    }
    if (gbm_ && owns_gbm_)
        gbm_device_destroy(static_cast<struct gbm_device*>(gbm_));
#endif
    gbm_ = nullptr;
    display_ = nullptr;
    context_ = nullptr;
    owns_gbm_ = false;
    renderer_[0] = '\0';
    gles_major_ = 0;
    gles_minor_ = 0;
}

lx::result<lx::gfx::egl_device> lx::gfx::egl_device::create(int drm_fd) {
#if defined(LUMEN_HAS_EGL)
    if (drm_fd < 0) {
        return lx::invalid_argument("egl_device::create needs an open DRM fd");
    }

    egl_device device;

    device.gbm_ = gbm_create_device(drm_fd);
    if (!device.gbm_)
        return lx::make_error(lx::gl_err::target_failed, "gbm_create_device failed");
    device.owns_gbm_ = true;

    // eglGetPlatformDisplayEXT rather than eglGetDisplay: the latter guesses the platform
    // from the pointer and gets it wrong when more than one is available.
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!get_platform_display)
        return lx::make_error(lx::gl_err::no_context, "EGL_EXT_platform_base missing");

    EGLDisplay dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, device.gbm_, nullptr);
    if (dpy == EGL_NO_DISPLAY)
        return lx::make_error(lx::gl_err::no_context, "eglGetPlatformDisplay(GBM) failed");

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(dpy, &major, &minor))
        return lx::make_error(lx::gl_err::no_context, "eglInitialize failed");
    device.display_ = dpy;

    const char* extensions = eglQueryString(dpy, EGL_EXTENSIONS);
    const bool surfaceless = detail::egl_has_extension(extensions, "EGL_KHR_surfaceless_context");
    const bool no_config = detail::egl_has_extension(extensions, "EGL_KHR_no_config_context");
    device.has_dmabuf_import_ =
        detail::egl_has_extension(extensions, "EGL_EXT_image_dma_buf_import");
    device.has_native_fence_ =
        detail::egl_has_extension(extensions, "EGL_ANDROID_native_fence_sync");

    if (!surfaceless) {
        // Without it every context needs a window surface, which would mean binding the
        // context to one output's gbm_surface and giving up the scanout slot rotation.
        return lx::make_error(lx::gl_err::no_context, "EGL_KHR_surfaceless_context missing");
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API))
        return lx::make_error(lx::gl_err::no_context, "eglBindAPI(OpenGL ES) failed");

    // GLES 3.1 when the driver has it — what the UI primitive renderer (`:ui_gl`) needs —
    // else the GLES 2 context the compositor's blit path has always run on (its shaders are
    // `#version 100`, valid in both). A config without ES3 in its renderable types cannot
    // hold a 3.x context, so the ES3 config is asked for first.
    const auto choose = [&](EGLint renderable, EGLConfig& out) {
        const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                                       renderable,       EGL_RED_SIZE,   8,
                                       EGL_GREEN_SIZE,   8,              EGL_BLUE_SIZE,
                                       8,                EGL_NONE};
        EGLint config_count = 0;
        return eglChooseConfig(dpy, config_attrs, &out, 1, &config_count) && config_count > 0;
    };
    static constexpr EGLint k_es3_bit = 0x0040; // EGL_OPENGL_ES3_BIT(_KHR)
    EGLContext ctx = EGL_NO_CONTEXT;
    {
        EGLConfig config = EGL_NO_CONFIG_KHR;
        if (no_config || choose(k_es3_bit, config)) {
            static const EGLint es31_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                                                EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
            ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, es31_attrs);
        }
    }
    if (ctx == EGL_NO_CONTEXT) {
        EGLConfig config = EGL_NO_CONFIG_KHR;
        if (!no_config && !choose(EGL_OPENGL_ES2_BIT, config))
            return lx::make_error(lx::gl_err::no_context, "eglChooseConfig found no usable config");
        static const EGLint es2_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
        ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, es2_attrs);
    }
    if (ctx == EGL_NO_CONTEXT)
        return lx::make_error(lx::gl_err::no_context, "eglCreateContext failed");
    device.context_ = ctx;

    if (auto current = device.make_current(); !current)
        return current.get_error();

    return device;
#else
    (void)drm_fd;
    return lx::not_implemented("lx::gfx::egl_device::create");
#endif
}

lx::result<void> lx::gfx::egl_device::make_current() {
#if defined(LUMEN_HAS_EGL)
    if (!display_ || !context_)
        return lx::not_implemented("lx::gfx::egl_device::make_current");
    if (!eglMakeCurrent(static_cast<EGLDisplay>(display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                        static_cast<EGLContext>(context_)))
        return lx::make_error(lx::gl_err::no_context, "eglMakeCurrent failed");

    if (renderer_[0] == '\0') {
        const auto* name = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (name) {
            std::strncpy(renderer_, name, sizeof(renderer_) - 1);
            renderer_[sizeof(renderer_) - 1] = '\0';
        }
        software_ = detail::renderer_is_software(renderer_);
        // "OpenGL ES 3.2 Mesa …": what the context really is (a driver may give more than
        // was asked for, never less than a successful request).
        if (const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION))) {
            int maj = 0, min = 0;
            if (std::sscanf(version, "OpenGL ES %d.%d", &maj, &min) == 2) {
                gles_major_ = maj;
                gles_minor_ = min;
            }
        }
    }
    return {};
#else
    return lx::not_implemented("lx::gfx::egl_device::make_current");
#endif
}

void lx::gfx::egl_device::release_current() {
#if defined(LUMEN_HAS_EGL)
    if (display_)
        eglMakeCurrent(static_cast<EGLDisplay>(display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
#endif
}

// ── gl_scanout_target ────────────────────────────────────────────────────────

lx::gfx::gl_scanout_target::~gl_scanout_target() { destroy(); }

lx::gfx::gl_scanout_target::gl_scanout_target(gl_scanout_target&& other) noexcept {
    *this = std::move(other);
}

lx::gfx::gl_scanout_target& lx::gfx::gl_scanout_target::operator=(
    gl_scanout_target&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    bo_ = other.bo_;
    image_ = other.image_;
    display_ = other.display_;
    renderbuffer_ = other.renderbuffer_;
    framebuffer_ = other.framebuffer_;
    width_ = other.width_;
    height_ = other.height_;
    format_ = other.format_;
    other.bo_ = nullptr;
    other.image_ = nullptr;
    other.display_ = nullptr;
    other.renderbuffer_ = 0;
    other.framebuffer_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    return *this;
}

void lx::gfx::gl_scanout_target::destroy() {
#if defined(LUMEN_HAS_EGL)
    if (framebuffer_)
        glDeleteFramebuffers(1, &framebuffer_);
    if (renderbuffer_)
        glDeleteRenderbuffers(1, &renderbuffer_);
    if (image_ && display_) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR")))
            destroy_image(static_cast<EGLDisplay>(display_), static_cast<EGLImageKHR>(image_));
    }
    if (bo_)
        gbm_bo_destroy(static_cast<struct gbm_bo*>(bo_));
#endif
    bo_ = nullptr;
    image_ = nullptr;
    display_ = nullptr;
    renderbuffer_ = 0;
    framebuffer_ = 0;
    width_ = 0;
    height_ = 0;
}

lx::result<lx::gfx::gl_scanout_target> lx::gfx::gl_scanout_target::create(const egl_device& device,
                                                                          unsigned width,
                                                                          unsigned height,
                                                                          lx::fourcc format) {
#if defined(LUMEN_HAS_EGL)
    if (!device.valid() || width == 0 || height == 0) {
        return lx::invalid_argument("gl_scanout_target::create: invalid parameters");
    }

    gl_scanout_target target;
    target.display_ = device.egl_display();
    target.width_ = width;
    target.height_ = height;
    target.format_ = format;

    auto* gbm = static_cast<struct gbm_device*>(device.gbm_device_handle());
    // SCANOUT is what makes this flippable; without RENDERING the driver may pick a layout
    // GL cannot draw into.
    target.bo_ = gbm_bo_create(gbm, width, height, static_cast<uint32_t>(format),
                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!target.bo_)
        return lx::make_error(lx::gl_err::target_failed, "gbm_bo_create(SCANOUT|RENDERING) failed");

    auto create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_to_renderbuffer =
        reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
            eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (!create_image || !image_to_renderbuffer)
        return lx::make_error(lx::gl_err::target_failed,
                              "EGLImage renderbuffer entrypoints missing");

    EGLImageKHR image = create_image(static_cast<EGLDisplay>(target.display_), EGL_NO_CONTEXT,
                                     EGL_NATIVE_PIXMAP_KHR, target.bo_, nullptr);
    if (image == EGL_NO_IMAGE_KHR)
        return lx::make_error(lx::gl_err::target_failed, "eglCreateImageKHR from gbm_bo failed");
    target.image_ = image;

    glGenRenderbuffers(1, &target.renderbuffer_);
    glBindRenderbuffer(GL_RENDERBUFFER, target.renderbuffer_);
    image_to_renderbuffer(GL_RENDERBUFFER, image);

    glGenFramebuffers(1, &target.framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              target.renderbuffer_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        return lx::make_error(lx::gl_err::target_failed, "scanout FBO incomplete");

    return target;
#else
    (void)device;
    (void)width;
    (void)height;
    (void)format;
    return lx::not_implemented("lx::gfx::gl_scanout_target::create");
#endif
}

lx::result<lx::gfx::gl_scanout_dmabuf> lx::gfx::gl_scanout_target::export_dmabuf() const {
#if defined(LUMEN_HAS_EGL)
    if (!bo_)
        return lx::not_implemented("lx::gfx::gl_scanout_target::export_dmabuf");

    auto* bo = static_cast<struct gbm_bo*>(bo_);
    if (gbm_bo_get_plane_count(bo) != 1) {
        return lx::make_error(lx::drm_err::mode_invalid,
                              "multi-plane scanout buffer is not supported");
    }

    const int fd = gbm_bo_get_fd(bo);
    if (fd < 0)
        return lx::make_error(lx::gl_err::target_failed, "gbm_bo_get_fd failed");

    gl_scanout_dmabuf out{};
    out.fd.reset(fd);
    out.width = width_;
    out.height = height_;
    out.stride = gbm_bo_get_stride(bo);
    out.offset = gbm_bo_get_offset(bo, 0);
    out.modifier = gbm_bo_get_modifier(bo);
    out.format = format_;
    return out;
#else
    return lx::not_implemented("lx::gfx::gl_scanout_target::export_dmabuf");
#endif
}

// ── gl_compositor ────────────────────────────────────────────────────────────

lx::gfx::gl_compositor::~gl_compositor() { shutdown(); }

int lx::gfx::gl_compositor::find(unsigned texture_id) const {
    if (texture_id == 0)
        return -1;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used && textures_[i].id == texture_id)
            return static_cast<int>(i);
    }
    return -1;
}

int lx::gfx::gl_compositor::alloc(unsigned texture_id) {
    if (const int existing = find(texture_id); existing >= 0)
        return existing;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used)
            continue;
        textures_[i] = {};
        textures_[i].used = true;
        textures_[i].id = texture_id;
        return static_cast<int>(i);
    }
    return -1;
}

void lx::gfx::gl_compositor::destroy_slot(texture_slot& slot) {
#if defined(LUMEN_HAS_EGL)
    for (unsigned& name : slot.names) {
        if (name)
            glDeleteTextures(1, &name);
        name = 0;
    }
    if (slot.image && device_ && device_->egl_display()) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR"))) {
            destroy_image(static_cast<EGLDisplay>(device_->egl_display()),
                          static_cast<EGLImageKHR>(slot.image));
        }
    }
#endif
    slot = {};
}

void lx::gfx::gl_compositor::forget_texture(unsigned texture_id) {
    if (const int index = find(texture_id); index >= 0)
        destroy_slot(textures_[static_cast<unsigned>(index)]);
}

lx::result<void> lx::gfx::gl_compositor::initialize(egl_device& device) {
#if defined(LUMEN_HAS_EGL)
    if (!device.valid())
        return lx::not_implemented("lx::gfx::gl_compositor::initialize");
    device_ = &device;

    const GLuint vertex = detail::compile_shader(GL_VERTEX_SHADER, detail::k_vertex_shader);
    if (!vertex)
        return lx::make_error(lx::gl_err::program_failed,
                              "composite vertex shader failed to compile");
    for (unsigned v = 0; v < 2; ++v) {
        const GLuint fragment = detail::compile_shader(
            GL_FRAGMENT_SHADER, detail::k_fragment_shader,
            v == k_program_identity ? "#define LUMEN_IDENTITY 1\n" : "\n");
        if (!fragment) {
            glDeleteShader(vertex);
            shutdown();
            return lx::make_error(lx::gl_err::program_failed,
                                  "composite fragment shader failed to compile");
        }
        const GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glBindAttribLocation(program, 0, "corner");
        glLinkProgram(program);
        glDeleteShader(fragment);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            glDeleteProgram(program);
            glDeleteShader(vertex);
            shutdown();
            return lx::make_error(lx::gl_err::program_failed, "composite program failed to link");
        }
        composite_program& p = programs_[v];
        p.id = program;
        p.rect = glGetUniformLocation(program, "rect");
        p.target = glGetUniformLocation(program, "target");
        p.opacity = glGetUniformLocation(program, "opacity");
        p.uv_origin_dx = glGetUniformLocation(program, "uv_origin_dx");
        p.uv_dy = glGetUniformLocation(program, "uv_dy");
        p.tint = glGetUniformLocation(program, "tint");
        p.src_bounds = glGetUniformLocation(program, "src_bounds");
        p.transfer = glGetUniformLocation(program, "transfer");
        p.alpha_one = glGetUniformLocation(program, "alpha_one");
        p.use_solid = glGetUniformLocation(program, "use_solid");
        p.solid_texel = glGetUniformLocation(program, "solid_texel");
        p.sampler = glGetUniformLocation(program, "surface");
    }
    glDeleteShader(vertex);

    const char* gl_ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    bgra_ = detail::egl_has_extension(gl_ext, "GL_EXT_texture_format_BGRA8888");
    unpack_subimage_ = detail::egl_has_extension(gl_ext, "GL_EXT_unpack_subimage");
    // "OpenGL ES 3.1 Mesa …": the context is created as ES 2, and drivers hand out the
    // newest compatible version, so ES 3's glBlitFramebuffer is there when it says 3.
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    target_copy_ = version && std::strncmp(version, "OpenGL ES ", 10) == 0 &&
                   std::atoi(version + 10) >= 3;

    // Unit quad; the vertex shader maps it onto each draw's destination rect.
    static const float corners[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};
    glGenBuffers(1, &vertex_buffer_);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return {};
#else
    (void)device;
    return lx::not_implemented("lx::gfx::gl_compositor::initialize");
#endif
}

void lx::gfx::gl_compositor::shutdown() {
#if defined(LUMEN_HAS_EGL)
    for (auto& slot : textures_) {
        if (slot.used)
            destroy_slot(slot);
    }
    if (vertex_buffer_)
        glDeleteBuffers(1, &vertex_buffer_);
    for (composite_program& p : programs_)
        if (p.id)
            glDeleteProgram(p.id);
#endif
    vertex_buffer_ = 0;
    for (composite_program& p : programs_)
        p = {};
    device_ = nullptr;
}

lx::result<void> lx::gfx::gl_compositor::upload_rgba_region(unsigned texture_id,
                                                            unsigned width, unsigned height,
                                                            lx::rect2i region,
                                                            const unsigned char* rgba) {
    const bool whole = region.width <= 0 || region.height <= 0 ||
                       (region.x == 0 && region.y == 0 &&
                        static_cast<unsigned>(region.width) == width &&
                        static_cast<unsigned>(region.height) == height);
    if (whole)
        return upload_rgba(texture_id, width, height, rgba, 0);
#if defined(LUMEN_HAS_EGL)
    if (!ready() || !rgba || width == 0 || height == 0 || region.x < 0 || region.y < 0 ||
        static_cast<unsigned>(region.x + region.width) > width ||
        static_cast<unsigned>(region.y + region.height) > height)
        return lx::invalid_argument("gl region upload: bad arguments");
    const int index = alloc(texture_id);
    if (index < 0)
        return lx::invalid_argument("gl texture table full");
    auto& slot = textures_[static_cast<unsigned>(index)];
    if (slot.is_dmabuf) {
        destroy_slot(slot);
        slot.used = true;
        slot.id = texture_id;
    }
    // Same alternation as a whole upload: never write the name the previous frame samples.
    slot.current ^= 1u;
    unsigned& name = slot.names[slot.current];
    const bool fresh = !name || slot.name_width[slot.current] != width ||
                       slot.name_height[slot.current] != height ||
                       slot.name_format[slot.current] != GL_RGBA;
    if (!name) {
        glGenTextures(1, &name);
        if (!name)
            return lx::make_error(lx::gl_err::import_failed, "glGenTextures failed");
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, name);
    }
    if (fresh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        ++partial_into_fresh_; // reported by the caller: the rest of this texture is undefined
    }
    // Rows are packed RGBA (4-byte aligned), so GLES 2's default unpack state fits.
    glTexSubImage2D(GL_TEXTURE_2D, 0, region.x, region.y, region.width, region.height,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    slot.name_width[slot.current] = width;
    slot.name_height[slot.current] = height;
    slot.name_format[slot.current] = GL_RGBA;
    slot.name_opaque[slot.current] = false;
    slot.width = width;
    slot.height = height;
    slot.is_dmabuf = false;
    return {};
#else
    (void)region;
    return lx::not_implemented("lx::gfx::gl_compositor::upload_rgba_region");
#endif
}

lx::result<void> lx::gfx::gl_compositor::upload_client_region(unsigned texture_id,
                                                              unsigned width, unsigned height,
                                                              lx::rect2i region,
                                                              const unsigned char* pixels,
                                                              unsigned stride, bool opaque) {
    return upload_client_regions(texture_id, width, height, region, {}, pixels, stride, opaque);
}

lx::result<void> lx::gfx::gl_compositor::upload_client_regions(
    unsigned texture_id, unsigned width, unsigned height, lx::rect2i region,
    std::span<const lx::rect2i> parts, const unsigned char* pixels, unsigned stride,
    bool opaque) {
#if defined(LUMEN_HAS_EGL)
    if (!ready() || !supports_client_upload() || !pixels || width == 0 || height == 0 ||
        stride % 4u != 0)
        return lx::invalid_argument("gl client upload: bad arguments");
    const bool whole = region.width <= 0 || region.height <= 0 ||
                       (region.x == 0 && region.y == 0 &&
                        static_cast<unsigned>(region.width) == width &&
                        static_cast<unsigned>(region.height) == height);
    if (whole)
        region = {0, 0, static_cast<int>(width), static_cast<int>(height)};
    if (region.x < 0 || region.y < 0 || static_cast<unsigned>(region.x + region.width) > width ||
        static_cast<unsigned>(region.y + region.height) > height)
        return lx::invalid_argument("gl client upload: region outside the buffer");
    if (stride < static_cast<unsigned>(region.width) * 4u)
        return lx::invalid_argument("gl client upload: stride shorter than a row");
    const int index = alloc(texture_id);
    if (index < 0)
        return lx::invalid_argument("gl texture table full");
    auto& slot = textures_[static_cast<unsigned>(index)];
    if (slot.is_dmabuf) {
        destroy_slot(slot);
        slot.used = true;
        slot.id = texture_id;
    }
    // Same alternation as the staged uploads: never write the name the previous frame
    // samples.
    slot.current ^= 1u;
    unsigned& name = slot.names[slot.current];
    const bool fresh = !name || slot.name_width[slot.current] != width ||
                       slot.name_height[slot.current] != height ||
                       slot.name_format[slot.current] != GL_BGRA_EXT;
    if (!name) {
        glGenTextures(1, &name);
        if (!name)
            return lx::make_error(lx::gl_err::import_failed, "glGenTextures failed");
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, name);
    }
    // The client's rows, at the client's stride: one pass by the driver, nothing staged.
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, static_cast<GLint>(stride / 4u));
    const unsigned char* first = pixels;
    if (fresh && whole) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, first);
    } else {
        if (fresh) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height), 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                         nullptr);
            ++partial_into_fresh_; // reported by the caller: the rest is undefined
        }
        if (parts.empty() || whole) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, region.x, region.y, region.width, region.height,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, first);
        } else {
            for (const lx::rect2i& r : parts) {
                // Clipped to the region, which the pointer arithmetic is relative to.
                const int x0 = std::max(r.x, region.x);
                const int y0 = std::max(r.y, region.y);
                const int x1 = std::min(r.x + r.width, region.x + region.width);
                const int y1 = std::min(r.y + r.height, region.y + region.height);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                const unsigned char* at =
                    first + static_cast<std::size_t>(y0 - region.y) * stride +
                    static_cast<std::size_t>(x0 - region.x) * 4u;
                glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x1 - x0, y1 - y0, GL_BGRA_EXT,
                                GL_UNSIGNED_BYTE, at);
            }
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    slot.name_width[slot.current] = width;
    slot.name_height[slot.current] = height;
    slot.name_format[slot.current] = GL_BGRA_EXT;
    slot.name_opaque[slot.current] = opaque;
    slot.width = width;
    slot.height = height;
    slot.is_dmabuf = false;
    return {};
#else
    (void)texture_id;
    (void)width;
    (void)height;
    (void)region;
    (void)parts;
    (void)pixels;
    (void)stride;
    (void)opaque;
    return lx::not_implemented("lx::gfx::gl_compositor::upload_client_regions");
#endif
}

lx::result<void> lx::gfx::gl_compositor::upload_rgba(unsigned texture_id, unsigned width,
                                                     unsigned height, const unsigned char* rgba,
                                                     unsigned stride) {
#if defined(LUMEN_HAS_EGL)
    if (!ready() || !rgba || width == 0 || height == 0)
        return lx::invalid_argument("gl upload: bad arguments");
    if (stride != 0 && stride != width * 4u) {
        // GLES 2 has no GL_UNPACK_ROW_LENGTH, so a padded source cannot be uploaded in one
        // call. Staged shm rows are packed; anything else is a caller bug worth naming.
        return lx::invalid_argument("gl upload: padded source stride is not supported");
    }

    const int index = alloc(texture_id);
    if (index < 0)
        return lx::invalid_argument("gl texture table full");
    auto& slot = textures_[static_cast<unsigned>(index)];
    if (slot.is_dmabuf) {
        // The client switched from dma-buf to shm for this id; drop the EGLImage before
        // reusing the slot for uploads.
        destroy_slot(slot);
        slot.used = true;
        slot.id = texture_id;
    }

    // Alternate names: uploading into the texture the previous frame is still sampling
    // stalls the driver until that draw retires, which on a virtualised GPU costs far more
    // than the upload itself.
    slot.current ^= 1u;
    unsigned& name = slot.names[slot.current];

    // Per name: a client resizing every frame (an interactive resize) alternates sizes
    // across the two names, and a sub-image upload into storage of another size is a GL
    // error that leaves the previous frame's pixels — a mismatched frame on screen.
    const bool resized =
        slot.name_width[slot.current] != width || slot.name_height[slot.current] != height ||
        slot.name_format[slot.current] != GL_RGBA;
    if (!name) {
        glGenTextures(1, &name);
        if (!name)
            return lx::make_error(lx::gl_err::import_failed, "glGenTextures failed");
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        glBindTexture(GL_TEXTURE_2D, name);
        if (resized) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width),
                            static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    slot.name_width[slot.current] = width;
    slot.name_height[slot.current] = height;
    slot.name_format[slot.current] = GL_RGBA;
    slot.name_opaque[slot.current] = false;
    slot.width = width;
    slot.height = height;
    slot.is_dmabuf = false;
    return {};
#else
    (void)texture_id;
    (void)width;
    (void)height;
    (void)rgba;
    (void)stride;
    return lx::not_implemented("lx::gfx::gl_compositor::upload_rgba");
#endif
}

lx::result<void> lx::gfx::gl_compositor::import_dmabuf(unsigned texture_id,
                                                       const dmabuf_desc& desc) {
#if defined(LUMEN_HAS_EGL)
    if (!ready() || !device_)
        return lx::not_implemented("lx::gfx::gl_compositor::import_dmabuf");
    if (!device_->supports_dmabuf_import()) {
        return lx::make_error(lx::gl_err::import_failed, "EGL_EXT_image_dma_buf_import missing");
    }
    if (desc.plane_count == 0 || desc.planes[0].fd.get() < 0) {
        return lx::invalid_argument("gl import: dmabuf has no planes");
    }
    // The same buffer committed again: the EGLImage already samples its memory, and the
    // client's new pixels are visible through it. A texture id names one buffer for its
    // whole life (it is retired when the wl_buffer is destroyed), so only the size could
    // differ — and a buffer's size never changes. Re-creating the image every commit cost
    // an EGLImage and a texture per frame for nothing.
    if (const int known = find(texture_id); known >= 0) {
        const auto& slot = textures_[static_cast<unsigned>(known)];
        if (slot.is_dmabuf && slot.image && slot.width == desc.width &&
            slot.height == desc.height)
            return {};
    }

    auto create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_to_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_image || !image_to_texture)
        return lx::make_error(lx::gl_err::import_failed, "EGLImage texture entrypoints missing");

    // Single plane only: the packed 8:8:8:8 formats clients actually use for windows.
    EGLint attrs[] = {EGL_WIDTH,
                      static_cast<EGLint>(desc.width),
                      EGL_HEIGHT,
                      static_cast<EGLint>(desc.height),
                      EGL_LINUX_DRM_FOURCC_EXT,
                      static_cast<EGLint>(desc.format),
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      desc.planes[0].fd.get(),
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      static_cast<EGLint>(desc.planes[0].offset),
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      static_cast<EGLint>(desc.planes[0].stride),
                      EGL_NONE};

    EGLImageKHR image = create_image(static_cast<EGLDisplay>(device_->egl_display()),
                                     EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (image == EGL_NO_IMAGE_KHR)
        return lx::make_error(lx::gl_err::import_failed, "eglCreateImageKHR(dma_buf) failed");

    const int index = alloc(texture_id);
    if (index < 0) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR")))
            destroy_image(static_cast<EGLDisplay>(device_->egl_display()), image);
        return lx::invalid_argument("gl texture table full");
    }

    auto& slot = textures_[static_cast<unsigned>(index)];
    destroy_slot(slot);
    slot.used = true;
    slot.id = texture_id;
    slot.is_dmabuf = true;
    slot.image = image;
    slot.width = desc.width;
    slot.height = desc.height;
    slot.current = 0;

    glGenTextures(1, &slot.names[0]);
    glBindTexture(GL_TEXTURE_2D, slot.names[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_to_texture(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);

    return {};
#else
    (void)texture_id;
    (void)desc;
    return lx::not_implemented("lx::gfx::gl_compositor::import_dmabuf");
#endif
}

lx::result<lx::gfx::gl_composite_stats> lx::gfx::gl_compositor::composite(
    gl_scanout_target& target, lx::color clear, const blit_command* cmds, unsigned count,
    lx::rect2i damage) {
    const bool has_damage = damage.width > 0 && damage.height > 0;
    if (!target.valid()) {
        return lx::invalid_argument("gl composite into an invalid target");
    }
    return composite_rects(target.framebuffer(), target.width(), target.height(), clear, cmds,
                           count, has_damage ? &damage : nullptr, has_damage ? 1u : 0u);
}

lx::result<lx::gfx::gl_composite_stats> lx::gfx::gl_compositor::composite_region(
    gl_scanout_target& target, lx::color clear, const blit_command* cmds, unsigned count,
    const damage_region& damage) {
    if (!target.valid()) {
        return lx::invalid_argument("gl composite into an invalid target");
    }
    if (damage.full)
        return composite_rects(target.framebuffer(), target.width(), target.height(), clear,
                               cmds, count, nullptr, 0);
    if (damage.count == 0)
        return gl_composite_stats{};
    return composite_rects(target.framebuffer(), target.width(), target.height(), clear, cmds,
                           count, damage.rects, damage.count);
}

lx::result<lx::gfx::gl_composite_stats> lx::gfx::gl_compositor::composite_into(
    unsigned framebuffer, unsigned width, unsigned height, lx::color clear,
    const blit_command* cmds, unsigned count, const damage_region& damage, bool clear_first,
    bool fence, bool flush) {
    if (framebuffer == 0 || width == 0 || height == 0) {
        return lx::invalid_argument("gl composite into an invalid framebuffer");
    }
    if (damage.full)
        return composite_rects(framebuffer, width, height, clear, cmds, count, nullptr, 0,
                               clear_first, fence, flush);
    if (damage.count == 0)
        return gl_composite_stats{};
    return composite_rects(framebuffer, width, height, clear, cmds, count, damage.rects,
                           damage.count, clear_first, fence, flush);
}

lx::result<lx::gfx::gl_composite_stats> lx::gfx::gl_compositor::composite_rects(
    unsigned framebuffer, unsigned width_px, unsigned height_px, lx::color clear,
    const blit_command* cmds, unsigned count, const lx::rect2i* rects, unsigned rect_count,
    bool clear_first, bool fence, bool flush) {
#if defined(LUMEN_HAS_EGL)
    if (!ready())
        return lx::not_implemented("lx::gfx::gl_compositor::composite");

    gl_composite_stats stats{};
    const auto width = static_cast<GLsizei>(width_px);
    const auto height = static_cast<GLsizei>(height_px);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, width, height);

    // Pipeline state is per frame, not per rect: only the scissor changes between passes.
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glActiveTexture(GL_TEXTURE0);
    // Uniforms belong to a program: the per-frame ones are set in both. Whether a program
    // replaces the sample with `solid_texel` is then changed only when it changes.
    bool solid_bound[2] = {false, false};
    for (const composite_program& p : programs_) {
        glUseProgram(p.id);
        if (p.sampler >= 0)
            glUniform1i(p.sampler, 0);
        if (p.target >= 0)
            glUniform2f(p.target, static_cast<float>(width), static_cast<float>(height));
        if (p.use_solid >= 0)
            glUniform1f(p.use_solid, 0.f);
    }
    unsigned bound = k_program_identity;

    // Scissor and blend state, set only when it changes. Mesa does not drop a redundant
    // call: on SVGA3D a repeated glScissor measured 1.7 µs per draw, a changed one 4 µs, a
    // scissor-test toggle 7 µs and a blend toggle 2.6 µs.
    bool scissor_on = false;
    lx::rect2i scissor_rect{0, 0, -1, -1};
    bool blend_on = false;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    // Sources are premultiplied, so the source factor is one.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    const auto set_scissor = [&](bool on, lx::rect2i r) {
        if (on != scissor_on) {
            if (on)
                glEnable(GL_SCISSOR_TEST);
            else
                glDisable(GL_SCISSOR_TEST);
            scissor_on = on;
        }
        if (on && (r.x != scissor_rect.x || r.y != scissor_rect.y || r.width != scissor_rect.width ||
                   r.height != scissor_rect.height)) {
            glScissor(r.x, r.y, r.width, r.height);
            scissor_rect = r;
        }
    };
    const auto set_blend = [&](bool on) {
        if (on == blend_on)
            return;
        if (on)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        blend_on = on;
    };

    const unsigned passes = rect_count > 0 ? rect_count : 1u;
    for (unsigned pass = 0; pass < passes; ++pass) {
        const bool has_damage = rect_count > 0;
        const lx::rect2i damage = has_damage ? rects[pass] : lx::rect2i{};
        if (has_damage && (damage.width <= 0 || damage.height <= 0))
            continue;

        // Scissor to the damage rect so the clear and every draw stay inside it. Window
        // y = 0 is memory row 0, the top (see the vertex shader), so the top-left rect is
        // used as is.
        if (has_damage)
            set_scissor(true, {damage.x, damage.y < 0 ? 0 : damage.y, damage.width, damage.height});
        else
            set_scissor(false, {});
        if (clear_first) {
            glClearColor(clear.r, clear.g, clear.b, clear.a);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        for (unsigned i = 0; i < count && cmds; ++i) {
            const auto& cmd = cmds[i];
            if (cmd.occluded || draw_invisible(cmd)) {
                ++stats.draws_culled;
                continue;
            }
            const texture_slot* slot_ptr = nullptr;
            unsigned name = 0;
            if (!cmd.solid) {
                const int index = find(cmd.texture_id);
                if (index < 0) {
                    ++stats.draws_skipped;
                    continue;
                }
                slot_ptr = &textures_[static_cast<unsigned>(index)];
                name = slot_ptr->names[slot_ptr->current];
                if (!name) {
                    ++stats.draws_skipped;
                    continue;
                }
            }

            // The scissor is damage ∩ clip, in top-left space, which is also window space
            // here (row 0 is the top). The quad covers exactly dst, so dst needs no scissor:
            // it only culls draws that miss this rect, which is most of them once damage is
            // split into several small rects. Leaving dst out keeps the scissor the same for
            // every unclipped draw of a pass, so it is set once instead of once per draw.
            int sx0 = 0;
            int sy0 = 0;
            int sx1 = static_cast<int>(width);
            int sy1 = static_cast<int>(height);
            bool scissored = false;
            if (has_damage) {
                sx0 = damage.x;
                sy0 = damage.y;
                sx1 = damage.x + damage.width;
                sy1 = damage.y + damage.height;
                scissored = true;
            }
            if (cmd.clip.width > 0 && cmd.clip.height > 0) {
                if (cmd.clip.x > sx0) sx0 = cmd.clip.x;
                if (cmd.clip.y > sy0) sy0 = cmd.clip.y;
                if (cmd.clip.x + cmd.clip.width < sx1) sx1 = cmd.clip.x + cmd.clip.width;
                if (cmd.clip.y + cmd.clip.height < sy1) sy1 = cmd.clip.y + cmd.clip.height;
                scissored = true;
            }
            if (std::max(sx0, cmd.dst.x) >= std::min(sx1, cmd.dst.x + cmd.dst.width) ||
                std::max(sy0, cmd.dst.y) >= std::min(sy1, cmd.dst.y + cmd.dst.height)) {
                ++stats.draws_culled;
                continue;
            }
            // An opaque, untinted solid colour is a clear of its rect: no shader, no
            // blending, no fragment work beyond the fill (wlroots draws such rects with a
            // clear too). The clear ignores the quad, so its scissor must be dst itself.
            const bool solid_clear = cmd.solid && cmd.blend == lx::blend_mode::opaque &&
                                     cmd.opacity >= 1.f && cmd.tint.r >= 1.f &&
                                     cmd.tint.g >= 1.f && cmd.tint.b >= 1.f &&
                                     cmd.tint.a >= 1.f && cmd.solid_color.a >= 1.f;
            if (solid_clear) {
                if (cmd.dst.x > sx0) sx0 = cmd.dst.x;
                if (cmd.dst.y > sy0) sy0 = cmd.dst.y;
                if (cmd.dst.x + cmd.dst.width < sx1) sx1 = cmd.dst.x + cmd.dst.width;
                if (cmd.dst.y + cmd.dst.height < sy1) sy1 = cmd.dst.y + cmd.dst.height;
                scissored = true;
            }
            if (scissored) {
                if (sx0 < 0) sx0 = 0;
                if (sy0 < 0) sy0 = 0;
                if (sx1 > static_cast<int>(width)) sx1 = static_cast<int>(width);
                if (sy1 > static_cast<int>(height)) sy1 = static_cast<int>(height);
                if (sx1 <= sx0 || sy1 <= sy0) {
                    ++stats.draws_culled;
                    continue;
                }
                set_scissor(true, {sx0, sy0, sx1 - sx0, sy1 - sy0});
            } else {
                set_scissor(false, {});
            }

            if (solid_clear) {
                glClearColor(cmd.solid_color.r, cmd.solid_color.g, cmd.solid_color.b, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
                ++stats.draws_submitted;
                continue;
            }

            set_blend(cmd.blend != lx::blend_mode::opaque);

            const int transfer = cmd.src_transfer == lx::transfer_function::linear    ? 0
                                 : cmd.src_transfer == lx::transfer_function::gamma22 ? 2
                                                                                      : 1;
            const unsigned want = transfer == 1 && cmd.tint.r == 1.f && cmd.tint.g == 1.f &&
                                          cmd.tint.b == 1.f
                                      ? k_program_identity
                                      : k_program_linear;
            if (want != bound) {
                glUseProgram(programs_[want].id);
                bound = want;
            }
            const composite_program& prog = programs_[bound];
            if (cmd.solid != solid_bound[bound]) {
                solid_bound[bound] = cmd.solid;
                if (prog.use_solid >= 0)
                    glUniform1f(prog.use_solid, cmd.solid ? 1.f : 0.f);
            }
            if (cmd.solid) {
                if (prog.solid_texel >= 0)
                    glUniform4f(prog.solid_texel, cmd.solid_color.r, cmd.solid_color.g,
                                cmd.solid_color.b, cmd.solid_color.a);
            } else {
                glBindTexture(GL_TEXTURE_2D, name);
            }
            if (prog.rect >= 0) {
                glUniform4f(prog.rect, static_cast<float>(cmd.dst.x), static_cast<float>(cmd.dst.y),
                            static_cast<float>(cmd.dst.width), static_cast<float>(cmd.dst.height));
            }
            if (prog.opacity >= 0)
                glUniform1f(prog.opacity, cmd.opacity);
            // The source box (empty: the whole texture) in normalized texture coordinates,
            // turned by the buffer transform. On a 1:1 draw at whole pixels every fragment
            // centre lands on a texel centre, so linear filtering is exact; a resampled
            // draw filters linearly (wlroots, KWin).
            float u0 = 0.f, v0 = 0.f, du = 1.f, dv = 1.f;
            float u_lo = 0.f, v_lo = 0.f, u_hi = 1.f, v_hi = 1.f;
            if (slot_ptr && slot_ptr->width > 0 && slot_ptr->height > 0) {
                const texture_slot& slot = *slot_ptr;
                const float tw = static_cast<float>(slot.width);
                const float th = static_cast<float>(slot.height);
                const lx::rect2f box = effective_source(cmd.src, slot.width, slot.height);
                u0 = box.x / tw;
                v0 = box.y / th;
                du = box.width / tw;
                dv = box.height / th;
                // Half-texel inset, so a magnified crop cannot sample across its own edge.
                const float half_u = 0.5f / tw;
                const float half_v = 0.5f / th;
                u_lo = u0 + half_u;
                v_lo = v0 + half_v;
                u_hi = u0 + du - half_u;
                v_hi = v0 + dv - half_v;
                if (u_lo > u_hi) u_lo = u_hi = 0.5f * (u_lo + u_hi);
                if (v_lo > v_hi) v_lo = v_hi = 0.5f * (v_lo + v_hi);
            }
            {
                const source_basis b = source_basis_for(cmd.buffer_xform);
                if (prog.uv_origin_dx >= 0)
                    glUniform4f(prog.uv_origin_dx, u0 + b.origin[0] * du,
                                v0 + b.origin[1] * dv, b.dx[0] * du, b.dx[1] * dv);
                if (prog.uv_dy >= 0)
                    glUniform2f(prog.uv_dy, b.dy[0] * du, b.dy[1] * dv);
            }
            if (prog.src_bounds >= 0)
                glUniform4f(prog.src_bounds, u_lo, v_lo, u_hi, v_hi);
            if (prog.tint >= 0)
                glUniform4f(prog.tint, cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a);
            if (prog.transfer >= 0)
                glUniform1i(prog.transfer, transfer);
            if (prog.alpha_one >= 0)
                glUniform1f(prog.alpha_one,
                            slot_ptr && slot_ptr->name_opaque[slot_ptr->current] ? 1.f : 0.f);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            ++stats.draws_submitted;
        }
    }

    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!fence) {
        // Part of a larger piece of work (a screen capture, a UI stream) that fences itself
        // once.
        if (flush)
            glFlush();
        if (const GLenum err = glGetError(); err != GL_NO_ERROR)
            return lx::make_error(lx::gl_err::draw_failed, "GL error during composite");
        return stats;
    }

    stats.out_fence_fd = detail::gl_frame_fence(device_);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        if (stats.out_fence_fd >= 0) {
            ::close(stats.out_fence_fd);
            stats.out_fence_fd = -1;
        }
        return lx::make_error(lx::gl_err::draw_failed, "GL error during composite");
    }

    return stats;
#else
    (void)framebuffer;
    (void)width_px;
    (void)height_px;
    (void)clear;
    (void)cmds;
    (void)count;
    (void)rects;
    (void)rect_count;
    (void)clear_first;
    (void)fence;
    (void)flush;
    return lx::not_implemented("lx::gfx::gl_compositor::composite");
#endif
}

lx::result<int> lx::gfx::gl_compositor::fence_frame() {
#if defined(LUMEN_HAS_EGL)
    if (!ready())
        return lx::not_implemented("lx::gfx::gl_compositor::fence_frame");
    const int fd = detail::gl_frame_fence(device_);
    if (glGetError() != GL_NO_ERROR) {
        if (fd >= 0)
            ::close(fd);
        return lx::make_error(lx::gl_err::draw_failed, "GL error before the frame fence");
    }
    return fd;
#else
    return lx::not_implemented("lx::gfx::gl_compositor::fence_frame");
#endif
}

#if defined(LUMEN_HAS_EGL)
int lx::gfx::detail::gl_frame_fence(egl_device* device) {
    int out_fence_fd = -1;
    // The KMS flip that follows reads this buffer from outside GL, and nothing synchronises
    // the two. Rather than block the render thread until the GPU is done, export the
    // frame's completion as a sync_file and let the atomic commit wait on it — the display
    // controller holds the flip back instead of the compositor holding the frame back.
    if (device && device->supports_native_fence()) {
        auto dpy = static_cast<EGLDisplay>(device->egl_display());
        auto create_sync =
            reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        auto destroy_sync =
            reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        auto dup_fence = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            eglGetProcAddress("eglDupNativeFenceFDANDROID"));

        if (create_sync && destroy_sync && dup_fence) {
            EGLSyncKHR sync = create_sync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
            if (sync != EGL_NO_SYNC_KHR) {
                // The fence is only real once the commands behind it have been submitted;
                // the extension requires a flush between creating it and dup'ing the FD.
                glFlush();
                const EGLint fd = dup_fence(dpy, sync);
                destroy_sync(dpy, sync);
                if (fd != EGL_NO_NATIVE_FENCE_FD_ANDROID)
                    out_fence_fd = fd;
            }
        }
    }

    // No usable fence — fall back to blocking rather than committing a buffer the GPU may
    // still be drawing into, which would show a partly-drawn frame.
    if (out_fence_fd < 0)
        glFinish();
    return out_fence_fd;
}
#endif

lx::result<void> lx::gfx::gl_compositor::copy_region(const gl_scanout_target& src,
                                                     gl_scanout_target& dst,
                                                     const damage_region& region) {
#if defined(LUMEN_HAS_EGL)
    if (!target_copy_ || !src.valid() || !dst.valid())
        return lx::not_implemented("lx::gfx::gl_compositor::copy_region");
    if (src.width() != dst.width() || src.height() != dst.height())
        return lx::invalid_argument("gl copy_region between targets of different sizes");
    const lx::rect2i whole{0, 0, static_cast<int>(src.width()), static_cast<int>(src.height())};
    damage_region r = region;
    if (r.full) {
        r.clear();
        r.add(whole);
    }
    r.clip(whole);
    if (r.count == 0)
        return {};
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.framebuffer());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst.framebuffer());
    // Both have row 0 on top: a rect lands where it was.
    for (unsigned i = 0; i < r.count; ++i) {
        const lx::rect2i& q = r.rects[i];
        glBlitFramebuffer(q.x, q.y, q.x + q.width, q.y + q.height, q.x, q.y, q.x + q.width,
                          q.y + q.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    if (glGetError() != GL_NO_ERROR)
        return lx::make_error(lx::gl_err::draw_failed, "glBlitFramebuffer between scanout targets failed");
    return {};
#else
    (void)src;
    (void)dst;
    (void)region;
    return lx::not_implemented("lx::gfx::gl_compositor::copy_region");
#endif
}

lx::result<void> lx::gfx::gl_compositor::read_back(const gl_scanout_target& target,
                                                   unsigned char* rgba, unsigned capacity) {
#if defined(LUMEN_HAS_EGL)
    if (!target.valid() || !rgba)
        return lx::not_implemented("lx::gfx::gl_compositor::read_back");
    const unsigned needed = target.width() * target.height() * 4u;
    if (capacity < needed) {
        return lx::invalid_argument("gl read_back: destination too small");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
    glReadPixels(0, 0, static_cast<GLsizei>(target.width()),
                 static_cast<GLsizei>(target.height()), GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR)
        return lx::make_error(lx::gl_err::draw_failed, "glReadPixels failed");

    // glReadPixels returns window y = 0 first, which for this framebuffer object is memory
    // row 0: the top line KMS scans. Rows are therefore already top-down and are returned
    // as the display sees them, not reordered.
    return {};
#else
    (void)target;
    (void)rgba;
    (void)capacity;
    return lx::not_implemented("lx::gfx::gl_compositor::read_back");
#endif
}
