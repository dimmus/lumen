module;

#if defined(LUMEN_HAS_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <cstring>

import lx.foundation;

export module lx.gfx:gl_renderer;

import :renderer;
import :dmabuf;

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

    [[nodiscard]] void* egl_display() const { return display_; }
    [[nodiscard]] void* gbm_device_handle() const { return gbm_; }

private:
    void destroy();

    void* gbm_ = nullptr;
    void* display_ = nullptr;
    void* context_ = nullptr;
    char renderer_[128]{};
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
    [[nodiscard]] bool ready() const { return program_ != 0; }

    /// Uploads CPU pixels in RGBA order. `stride` is bytes per row; 0 means packed.
    [[nodiscard]] lx::result<void> upload_rgba(unsigned texture_id, unsigned width,
                                               unsigned height, const unsigned char* rgba,
                                               unsigned stride = 0);

    /// Samples a client dma-buf in place via EGL_EXT_image_dma_buf_import.
    [[nodiscard]] lx::result<void> import_dmabuf(unsigned texture_id, const dmabuf_desc& desc);

    void forget_texture(unsigned texture_id);

    [[nodiscard]] lx::result<gl_composite_stats> composite(gl_scanout_target& target,
                                                           lx::color clear,
                                                           const blit_command* cmds,
                                                           unsigned count, lx::rect2i damage);

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

    egl_device* device_ = nullptr;
    unsigned program_ = 0;
    unsigned vertex_buffer_ = 0;
    int uniform_rect_ = -1;
    int uniform_target_ = -1;
    int uniform_opacity_ = -1;
    int uniform_src_uv_ = -1;
    int uniform_tint_ = -1;
    int uniform_src_bounds_ = -1;
    int uniform_transfer_ = -1;
    int uniform_sampler_ = -1;
    texture_slot textures_[k_max_textures]{};
};

} // namespace lx::gfx



namespace lx::gfx::detail {

#if defined(LUMEN_HAS_EGL)

lx::error gl_error(lx::gl_err code, const char* message) {
    return lx::make_error(lx::error_domain::gl, static_cast<int>(code), message);
}

/// GLES 2 is the floor every driver supports, and a compositor's shader needs nothing
/// beyond it: place a quad from a rect, sample one texture, scale by opacity.
constexpr const char* k_vertex_shader = R"(#version 100
attribute vec2 corner;
uniform vec4 rect;
uniform vec2 target;
uniform vec4 src_uv;
varying vec2 uv;
void main() {
    // Sample the source sub-rectangle, so cropping and fractional scale are the same
    // draw with different UVs rather than a separate path.
    uv = src_uv.xy + corner * src_uv.zw;
    vec2 px = rect.xy + corner * rect.zw;
    vec2 ndc = (px / target) * 2.0 - 1.0;
    // KMS scans top to bottom; GL's origin is bottom left.
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

constexpr const char* k_fragment_shader = R"(#version 100
precision mediump float;
varying vec2 uv;
uniform sampler2D surface;
uniform float opacity;
uniform vec4 tint;
uniform vec4 src_bounds;
// 0 linear, 1 sRGB, 2 gamma 2.2. GLES 2 has no HDR path, so PQ and HLG fall back to sRGB
// rather than pretending — the Vulkan backend is where HDR lands.
uniform int transfer;

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
    vec2 s = clamp(uv, src_bounds.xy, src_bounds.zw);
    vec4 texel = texture2D(surface, s);

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
}
)";

unsigned compile_shader(unsigned kind, const char* source) {
    const GLuint shader = glCreateShader(kind);
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, nullptr);
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
    *this = static_cast<egl_device&&>(other);
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
}

lx::result<lx::gfx::egl_device> lx::gfx::egl_device::create(int drm_fd) {
#if defined(LUMEN_HAS_EGL)
    if (drm_fd < 0) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "egl_device::create needs an open DRM fd");
    }

    egl_device device;

    device.gbm_ = gbm_create_device(drm_fd);
    if (!device.gbm_)
        return detail::gl_error(lx::gl_err::target_failed, "gbm_create_device failed");
    device.owns_gbm_ = true;

    // eglGetPlatformDisplayEXT rather than eglGetDisplay: the latter guesses the platform
    // from the pointer and gets it wrong when more than one is available.
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!get_platform_display)
        return detail::gl_error(lx::gl_err::no_context, "EGL_EXT_platform_base missing");

    EGLDisplay dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, device.gbm_, nullptr);
    if (dpy == EGL_NO_DISPLAY)
        return detail::gl_error(lx::gl_err::no_context, "eglGetPlatformDisplay(GBM) failed");

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(dpy, &major, &minor))
        return detail::gl_error(lx::gl_err::no_context, "eglInitialize failed");
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
        return detail::gl_error(lx::gl_err::no_context, "EGL_KHR_surfaceless_context missing");
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API))
        return detail::gl_error(lx::gl_err::no_context, "eglBindAPI(OpenGL ES) failed");

    EGLConfig config = EGL_NO_CONFIG_KHR;
    if (!no_config) {
        static const EGLint config_attrs[] = {EGL_SURFACE_TYPE,
                                              EGL_WINDOW_BIT,
                                              EGL_RENDERABLE_TYPE,
                                              EGL_OPENGL_ES2_BIT,
                                              EGL_RED_SIZE,
                                              8,
                                              EGL_GREEN_SIZE,
                                              8,
                                              EGL_BLUE_SIZE,
                                              8,
                                              EGL_NONE};
        EGLint config_count = 0;
        if (!eglChooseConfig(dpy, config_attrs, &config, 1, &config_count) || config_count == 0)
            return detail::gl_error(lx::gl_err::no_context, "eglChooseConfig found no usable config");
    }

    static const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
    if (ctx == EGL_NO_CONTEXT)
        return detail::gl_error(lx::gl_err::no_context, "eglCreateContext failed");
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
        return detail::gl_error(lx::gl_err::no_context, "eglMakeCurrent failed");

    if (renderer_[0] == '\0') {
        const auto* name = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (name) {
            std::strncpy(renderer_, name, sizeof(renderer_) - 1);
            renderer_[sizeof(renderer_) - 1] = '\0';
        }
        software_ = detail::renderer_is_software(renderer_);
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
    *this = static_cast<gl_scanout_target&&>(other);
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
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "gl_scanout_target::create: invalid parameters");
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
        return detail::gl_error(lx::gl_err::target_failed, "gbm_bo_create(SCANOUT|RENDERING) failed");

    auto create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_to_renderbuffer =
        reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
            eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (!create_image || !image_to_renderbuffer)
        return detail::gl_error(lx::gl_err::target_failed, "EGLImage renderbuffer entrypoints missing");

    EGLImageKHR image = create_image(static_cast<EGLDisplay>(target.display_), EGL_NO_CONTEXT,
                                     EGL_NATIVE_PIXMAP_KHR, target.bo_, nullptr);
    if (image == EGL_NO_IMAGE_KHR)
        return detail::gl_error(lx::gl_err::target_failed, "eglCreateImageKHR from gbm_bo failed");
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
        return detail::gl_error(lx::gl_err::target_failed, "scanout FBO incomplete");

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
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "multi-plane scanout buffer is not supported");
    }

    const int fd = gbm_bo_get_fd(bo);
    if (fd < 0)
        return detail::gl_error(lx::gl_err::target_failed, "gbm_bo_get_fd failed");

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
        return detail::gl_error(lx::gl_err::program_failed, "composite vertex shader failed to compile");
    const GLuint fragment = detail::compile_shader(GL_FRAGMENT_SHADER, detail::k_fragment_shader);
    if (!fragment) {
        glDeleteShader(vertex);
        return detail::gl_error(lx::gl_err::program_failed, "composite fragment shader failed to compile");
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "corner");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(program);
        return detail::gl_error(lx::gl_err::program_failed, "composite program failed to link");
    }
    program_ = program;

    uniform_rect_ = glGetUniformLocation(program, "rect");
    uniform_target_ = glGetUniformLocation(program, "target");
    uniform_opacity_ = glGetUniformLocation(program, "opacity");
    uniform_src_uv_ = glGetUniformLocation(program, "src_uv");
    uniform_tint_ = glGetUniformLocation(program, "tint");
    uniform_src_bounds_ = glGetUniformLocation(program, "src_bounds");
    uniform_transfer_ = glGetUniformLocation(program, "transfer");
    uniform_sampler_ = glGetUniformLocation(program, "surface");

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
    if (program_)
        glDeleteProgram(program_);
#endif
    vertex_buffer_ = 0;
    program_ = 0;
    device_ = nullptr;
}

lx::result<void> lx::gfx::gl_compositor::upload_rgba(unsigned texture_id, unsigned width,
                                                     unsigned height, const unsigned char* rgba,
                                                     unsigned stride) {
#if defined(LUMEN_HAS_EGL)
    if (!ready() || !rgba || width == 0 || height == 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "gl upload: bad arguments");
    if (stride != 0 && stride != width * 4u) {
        // GLES 2 has no GL_UNPACK_ROW_LENGTH, so a padded source cannot be uploaded in one
        // call. Staged shm rows are packed; anything else is a caller bug worth naming.
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "gl upload: padded source stride is not supported");
    }

    const int index = alloc(texture_id);
    if (index < 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "gl texture table full");
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

    const bool resized = slot.width != width || slot.height != height;
    if (!name) {
        glGenTextures(1, &name);
        if (!name)
            return detail::gl_error(lx::gl_err::import_failed, "glGenTextures failed");
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
        return detail::gl_error(lx::gl_err::import_failed,
                                "EGL_EXT_image_dma_buf_import missing");
    }
    if (desc.plane_count == 0 || desc.planes[0].fd.get() < 0) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "gl import: dmabuf has no planes");
    }

    auto create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_to_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_image || !image_to_texture)
        return detail::gl_error(lx::gl_err::import_failed, "EGLImage texture entrypoints missing");

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
        return detail::gl_error(lx::gl_err::import_failed, "eglCreateImageKHR(dma_buf) failed");

    const int index = alloc(texture_id);
    if (index < 0) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR")))
            destroy_image(static_cast<EGLDisplay>(device_->egl_display()), image);
        return lx::make_error(lx::error_domain::invalid_argument, 0, "gl texture table full");
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
#if defined(LUMEN_HAS_EGL)
    if (!ready())
        return lx::not_implemented("lx::gfx::gl_compositor::composite");
    if (!target.valid()) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "gl composite into an invalid target");
    }

    gl_composite_stats stats{};
    const auto width = static_cast<GLsizei>(target.width());
    const auto height = static_cast<GLsizei>(target.height());

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
    glViewport(0, 0, width, height);

    // Scissor to the damage rect so the clear and every draw stay inside it. GL's origin is
    // bottom left, so the rect is flipped to match the top-left space damage is expressed in.
    const bool has_damage = damage.width > 0 && damage.height > 0;
    if (has_damage) {
        const int y = height - (damage.y + damage.height);
        glEnable(GL_SCISSOR_TEST);
        glScissor(damage.x, y < 0 ? 0 : y, damage.width, damage.height);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program_);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glActiveTexture(GL_TEXTURE0);
    if (uniform_sampler_ >= 0)
        glUniform1i(uniform_sampler_, 0);
    if (uniform_target_ >= 0)
        glUniform2f(uniform_target_, static_cast<float>(width), static_cast<float>(height));

    for (unsigned i = 0; i < count && cmds; ++i) {
        const auto& cmd = cmds[i];
        const int index = find(cmd.texture_id);
        if (index < 0) {
            ++stats.draws_skipped;
            continue;
        }
        const auto& slot = textures_[static_cast<unsigned>(index)];
        const unsigned name = slot.names[slot.current];
        if (!name) {
            ++stats.draws_skipped;
            continue;
        }

        if (cmd.blend == lx::blend_mode::opaque) {
            glDisable(GL_BLEND);
        } else {
            glEnable(GL_BLEND);
            // Sources are premultiplied, so the source factor is one.
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }

        // Effective scissor is damage ∩ clip, computed in top-left space and flipped once
        // at the end. Set explicitly on every draw rather than only when a clip is present:
        // leaving the previous draw's scissor in place would silently clip the next one.
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
        if (scissored) {
            if (sx0 < 0) sx0 = 0;
            if (sy0 < 0) sy0 = 0;
            if (sx1 > static_cast<int>(width)) sx1 = static_cast<int>(width);
            if (sy1 > static_cast<int>(height)) sy1 = static_cast<int>(height);
            if (sx1 <= sx0 || sy1 <= sy0) {
                ++stats.draws_culled;
                continue;
            }
            glEnable(GL_SCISSOR_TEST);
            glScissor(sx0, static_cast<int>(height) - sy1, sx1 - sx0, sy1 - sy0);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glBindTexture(GL_TEXTURE_2D, name);
        if (uniform_rect_ >= 0) {
            glUniform4f(uniform_rect_, static_cast<float>(cmd.dst.x), static_cast<float>(cmd.dst.y),
                        static_cast<float>(cmd.dst.width), static_cast<float>(cmd.dst.height));
        }
        if (uniform_opacity_ >= 0)
            glUniform1f(uniform_opacity_, cmd.opacity);
        // An empty src means the whole texture, which is the identity rect.
        float u0 = 0.f, v0 = 0.f, du = 1.f, dv = 1.f;
        float u_lo = 0.f, v_lo = 0.f, u_hi = 1.f, v_hi = 1.f;
        if (slot.width > 0 && slot.height > 0) {
            const float tw = static_cast<float>(slot.width);
            const float th = static_cast<float>(slot.height);
            if (cmd.src.width > 0 && cmd.src.height > 0) {
                u0 = static_cast<float>(cmd.src.x) / tw;
                v0 = static_cast<float>(cmd.src.y) / th;
                du = static_cast<float>(cmd.src.width) / tw;
                dv = static_cast<float>(cmd.src.height) / th;
            }
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
        if (uniform_src_uv_ >= 0)
            glUniform4f(uniform_src_uv_, u0, v0, du, dv);
        if (uniform_src_bounds_ >= 0)
            glUniform4f(uniform_src_bounds_, u_lo, v_lo, u_hi, v_hi);
        if (uniform_tint_ >= 0)
            glUniform4f(uniform_tint_, cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a);
        if (uniform_transfer_ >= 0) {
            const int slot = cmd.src_transfer == lx::transfer_function::linear    ? 0
                             : cmd.src_transfer == lx::transfer_function::gamma22 ? 2
                                                                                  : 1;
            glUniform1i(uniform_transfer_, slot);
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        ++stats.draws_submitted;
    }

    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // The KMS flip that follows reads this buffer from outside GL, and nothing synchronises
    // the two. Rather than block the render thread until the GPU is done, export the
    // frame's completion as a sync_file and let the atomic commit wait on it — the display
    // controller holds the flip back instead of the compositor holding the frame back.
    if (device_ && device_->supports_native_fence()) {
        auto dpy = static_cast<EGLDisplay>(device_->egl_display());
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
                    stats.out_fence_fd = fd;
            }
        }
    }

    // No usable fence — fall back to blocking rather than committing a buffer the GPU may
    // still be drawing into, which would show a partly-drawn frame.
    if (stats.out_fence_fd < 0)
        glFinish();

    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        if (stats.out_fence_fd >= 0) {
            ::close(stats.out_fence_fd);
            stats.out_fence_fd = -1;
        }
        return detail::gl_error(lx::gl_err::draw_failed, "GL error during composite");
    }

    return stats;
#else
    (void)target;
    (void)clear;
    (void)cmds;
    (void)count;
    (void)damage;
    return lx::not_implemented("lx::gfx::gl_compositor::composite");
#endif
}

lx::result<void> lx::gfx::gl_compositor::read_back(const gl_scanout_target& target,
                                                   unsigned char* rgba, unsigned capacity) {
#if defined(LUMEN_HAS_EGL)
    if (!target.valid() || !rgba)
        return lx::not_implemented("lx::gfx::gl_compositor::read_back");
    const unsigned needed = target.width() * target.height() * 4u;
    if (capacity < needed) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "gl read_back: destination too small");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
    glReadPixels(0, 0, static_cast<GLsizei>(target.width()),
                 static_cast<GLsizei>(target.height()), GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR)
        return detail::gl_error(lx::gl_err::draw_failed, "glReadPixels failed");

    // glReadPixels hands back rows bottom-up; flip so row 0 is the top, matching every
    // other surface in the compositor.
    const unsigned stride = target.width() * 4u;
    for (unsigned y = 0; y < target.height() / 2; ++y) {
        unsigned char* top = rgba + static_cast<std::size_t>(y) * stride;
        unsigned char* bottom =
            rgba + static_cast<std::size_t>(target.height() - 1 - y) * stride;
        for (unsigned i = 0; i < stride; ++i) {
            const unsigned char tmp = top[i];
            top[i] = bottom[i];
            bottom[i] = tmp;
        }
    }
    return {};
#else
    (void)target;
    (void)rgba;
    (void)capacity;
    return lx::not_implemented("lx::gfx::gl_compositor::read_back");
#endif
}
