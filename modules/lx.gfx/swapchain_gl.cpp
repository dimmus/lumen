module;

// The GL tier of `lx.gfx:swapchain` (ADR-010 §4.10 item 2): gbm_bo from the feedback's
// modifiers → EGLImage → renderbuffer → FBO on the render node's GLES 3.1 context, drawn by
// :ui_gl. Explicit sync: the frame's EGL_ANDROID_native_fence_sync → sync_file → the image's
// DRM syncobj at the acquire point (a temporary binary syncobj, then a transfer — the
// timeline ops' `import_sync_file`, the libdrm calls Mesa's WSI makes); the release point
// is waited through an eventfd (DRM_IOCTL_SYNCOBJ_EVENTFD) the window's loop watches.
// Implicit sync: glFlush — the driver attaches the frame's fence to the dma-buf's
// reservation, which the compositor's import waits on — and wl_buffer.release.

#if defined(LUMEN_HAS_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#endif
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

module lx.gfx;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;
import :gl_renderer;
import :drm_timeline;
import :swapchain;

namespace lx::gfx {

namespace {

constexpr lx::trace::category log_swapchain_gl{"gfx.swapchain"};

[[maybe_unused]] std::uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

[[maybe_unused]] void copy_name(char* dst, std::size_t cap, const char* src) {
    std::size_t n = 0;
    if (src)
        for (; src[n] && n + 1 < cap; ++n)
            dst[n] = src[n];
    dst[n] = '\0';
}

#if defined(LUMEN_HAS_EGL)
[[nodiscard]] bool has_extension(const char* list, const char* name) {
    if (!list || !name)
        return false;
    const std::size_t n = std::strlen(name);
    for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
            return true;
    return false;
}

enum gl_fn : unsigned { fn_create_sync, fn_destroy_sync, fn_dup_fence, fn_count };
#endif

} // namespace

lx::result<void> client_gpu::try_gl(bool forced, bool allow_software) {
#if defined(LUMEN_HAS_EGL)
    auto dev = egl_device::create(node_.fd.get());
    if (!dev) {
        refuse(client_tier::gl, "EGL on GBM failed");
        return dev.get_error();
    }
    egl_ = std::move(dev).value();
    // A software rasterizer behind GL is the CPU renderer reached through an upload and a
    // driver stack (the compositor's refusal rule, AGENTS.md §1.1).
    if (egl_.is_software_renderer() && !(forced && allow_software)) {
        refuse(client_tier::gl, "software GL_RENDERER");
        egl_ = egl_device{};
        return lx::make_error(lx::gl_err::unsupported,
                              "GL is a software rasterizer: the CPU renderer is cheaper");
    }
    if (!egl_.gles_at_least(3, 1)) {
        refuse(client_tier::gl, "below GLES 3.1");
        egl_ = egl_device{};
        return lx::make_error(lx::gl_err::unsupported, ":ui_gl needs GLES 3.1");
    }
    software_ = egl_.is_software_renderer();
    copy_name(name_, sizeof(name_), egl_.renderer());
    return {};
#else
    (void)forced;
    (void)allow_software;
    refuse(client_tier::gl, "built without EGL");
    return lx::not_implemented("lx::gfx::client_gpu::try_gl");
#endif
}

lx::result<void> client_swapchain::make_timeline(slot& s) {
    unsigned handle = 0;
    if (!ops_.create || ops_.create(ops_.user, &handle) != 0)
        return lx::make_error(lx::drm_err::timeline_failed, "drmSyncobjCreate failed");
    int fd = -1;
    if (!ops_.handle_to_fd || ops_.handle_to_fd(ops_.user, handle, &fd) != 0 || fd < 0) {
        if (ops_.destroy)
            ops_.destroy(ops_.user, handle);
        return lx::make_error(lx::drm_err::timeline_failed, "drmSyncobjHandleToFD failed");
    }
    s.timeline.reset(fd);
    s.timeline_handle = handle;
    return {};
}

lx::result<void> client_swapchain::make_gl(slot& s) {
#if defined(LUMEN_HAS_EGL)
    egl_device* egl = gpu_ ? gpu_->egl() : nullptr;
    if (!egl || !egl->valid())
        return lx::not_implemented("lx::gfx::client_swapchain::make_gl");
    if (auto current = acquire_gl(s); !current)
        return current.get_error();
    auto dpy = static_cast<EGLDisplay>(egl->egl_display());
    if (!gl_fns_[fn_create_sync]) {
        gl_fns_[fn_create_sync] = reinterpret_cast<void*>(eglGetProcAddress("eglCreateSyncKHR"));
        gl_fns_[fn_destroy_sync] = reinterpret_cast<void*>(eglGetProcAddress("eglDestroySyncKHR"));
        gl_fns_[fn_dup_fence] =
            reinterpret_cast<void*>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    }

    auto* gbm = static_cast<struct gbm_device*>(egl->gbm_device_handle());
    std::uint64_t mods[k_client_max_modifiers]{};
    unsigned n = 0;
    bool implicit_ok = false;
    for (std::uint64_t m : options_.format.list()) {
        if (m == k_modifier_invalid)
            implicit_ok = true;
        else
            mods[n++] = m;
    }
    std::uint32_t usage = GBM_BO_USE_RENDERING;
    if (options_.format.scanout)
        usage |= GBM_BO_USE_SCANOUT;
    struct gbm_bo* bo = nullptr;
    unsigned path = 0;
    if (n > 0) {
        bo = gbm_bo_create_with_modifiers2(gbm, width_, height_, fourcc_, mods, n, usage);
        path = 2;
        if (!bo) {
            bo = gbm_bo_create_with_modifiers(gbm, width_, height_, fourcc_, mods, n);
            path = 1;
        }
    }
    bool forced_linear = false;
    if (!bo) {
        // No modifier-aware allocation: the driver's implicit layout, or linear when that is
        // all the compositor takes.
        const bool linear_only = n == 1 && mods[0] == k_modifier_linear;
        forced_linear = linear_only && !implicit_ok;
        if (!implicit_ok && !linear_only)
            return lx::make_error(lx::gl_err::target_failed,
                                  "gbm could not allocate any offered modifier");
        bo = gbm_bo_create(gbm, width_, height_, fourcc_,
                           usage | (forced_linear ? GBM_BO_USE_LINEAR : 0u));
        path = 0;
        if (bo && stats_.images_created == 0)
            log_swapchain_gl.info("gbm: no modifier allocation, gbm_bo_create with the {} "
                                  "modifier",
                                  forced_linear ? "linear" : "implicit");
    }
    if (!bo)
        return lx::make_error(lx::gl_err::target_failed, "gbm_bo_create failed");
    s.bo = bo;
    stats_.gbm_path = path;

    std::uint64_t modifier = gbm_bo_get_modifier(bo);
    if (path == 0)
        modifier = forced_linear ? k_modifier_linear : k_modifier_invalid;
    const int planes = gbm_bo_get_plane_count(bo);
    if (planes < 1 || planes > 4)
        return lx::make_error(lx::gl_err::export_failed, "gbm_bo has no usable planes");
    s.image.modifier = modifier;
    s.image.plane_count = static_cast<unsigned>(planes);
    for (int p = 0; p < planes; ++p) {
        const int fd = gbm_bo_get_fd_for_plane(bo, p);
        if (fd < 0)
            return lx::make_error(lx::gl_err::export_failed, "gbm_bo_get_fd_for_plane failed");
        s.plane_fds[p].reset(fd);
        s.image.planes[p] = {fd, gbm_bo_get_offset(bo, p), gbm_bo_get_stride_for_plane(bo, p)};
    }

    // The EGLImage from the very planes the compositor gets: if their offsets, strides or
    // modifier were wrong, this import fails here rather than on the compositor's side.
    auto create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_to_renderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (!create_image || !image_to_renderbuffer)
        return lx::make_error(lx::gl_err::target_failed, "EGLImage renderbuffer entry points missing");
    const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    if (has_extension(exts, "EGL_EXT_image_dma_buf_import")) {
        const bool with_modifier = modifier != k_modifier_invalid &&
                                   has_extension(exts, "EGL_EXT_image_dma_buf_import_modifiers");
        EGLint attrs[64];
        unsigned a = 0;
        attrs[a++] = EGL_WIDTH;
        attrs[a++] = static_cast<EGLint>(width_);
        attrs[a++] = EGL_HEIGHT;
        attrs[a++] = static_cast<EGLint>(height_);
        attrs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
        attrs[a++] = static_cast<EGLint>(fourcc_);
        static constexpr EGLint k_fd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                           EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
        static constexpr EGLint k_off[4] = {
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
        static constexpr EGLint k_pitch[4] = {
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
        static constexpr EGLint k_lo[4] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
        static constexpr EGLint k_hi[4] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
        for (int p = 0; p < planes; ++p) {
            attrs[a++] = k_fd[p];
            attrs[a++] = s.image.planes[p].fd;
            attrs[a++] = k_off[p];
            attrs[a++] = static_cast<EGLint>(s.image.planes[p].offset);
            attrs[a++] = k_pitch[p];
            attrs[a++] = static_cast<EGLint>(s.image.planes[p].stride);
            if (with_modifier) {
                attrs[a++] = k_lo[p];
                attrs[a++] = static_cast<EGLint>(modifier & 0xffffffffu);
                attrs[a++] = k_hi[p];
                attrs[a++] = static_cast<EGLint>(modifier >> 32);
            }
        }
        attrs[a++] = EGL_NONE;
        image = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    }
    if (image == EGL_NO_IMAGE_KHR)
        image = create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, nullptr);
    if (image == EGL_NO_IMAGE_KHR)
        return lx::make_error(lx::gl_err::target_failed, "EGLImage of the swapchain bo failed");
    s.egl_image = image;

    glGenRenderbuffers(1, &s.renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, s.renderbuffer);
    image_to_renderbuffer(GL_RENDERBUFFER, image);
    glGenFramebuffers(1, &s.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              s.renderbuffer);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        return lx::make_error(lx::gl_err::target_failed, "swapchain FBO incomplete");
    s.image.gl_framebuffer = s.framebuffer;

    if (explicit_) {
        if (auto t = make_timeline(s); !t) {
            log_swapchain_gl.warn("explicit sync: {} — implicit sync from here on",
                                  t.get_error().message);
            explicit_ = false;
        }
    }
    return {};
#else
    (void)s;
    return lx::not_implemented("lx::gfx::client_swapchain::make_gl");
#endif
}

void client_swapchain::destroy_gl(slot& s) {
#if defined(LUMEN_HAS_EGL)
    egl_device* egl = gpu_ ? gpu_->egl() : nullptr;
    if (egl && (s.framebuffer || s.renderbuffer || s.egl_image))
        (void)acquire_gl(s);
    if (s.framebuffer)
        glDeleteFramebuffers(1, &s.framebuffer);
    if (s.renderbuffer)
        glDeleteRenderbuffers(1, &s.renderbuffer);
    if (s.egl_image && egl) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR")))
            destroy_image(static_cast<EGLDisplay>(egl->egl_display()),
                          static_cast<EGLImageKHR>(s.egl_image));
    }
    if (s.bo)
        gbm_bo_destroy(static_cast<struct gbm_bo*>(s.bo));
#endif
    s.framebuffer = 0;
    s.renderbuffer = 0;
    s.egl_image = nullptr;
    s.bo = nullptr;
}

lx::result<void> client_swapchain::acquire_gl(slot&) {
#if defined(LUMEN_HAS_EGL)
    egl_device* egl = gpu_ ? gpu_->egl() : nullptr;
    if (!egl)
        return lx::not_implemented("lx::gfx::client_swapchain::acquire_gl");
    // Another context may be current on this thread (a second window, a test's reader).
    if (!gl_context_ || eglGetCurrentContext() != static_cast<EGLContext>(gl_context_)) {
        if (auto current = egl->make_current(); !current)
            return current.get_error();
        gl_context_ = eglGetCurrentContext();
    }
    return {};
#else
    return lx::not_implemented("lx::gfx::client_swapchain::acquire_gl");
#endif
}

lx::result<void> client_swapchain::present_gl(slot& s) {
#if defined(LUMEN_HAS_EGL)
    if (!explicit_) {
        // Implicit sync: submitted is enough — the driver fences the dma-buf.
        glFlush();
        return {};
    }
    auto dpy = static_cast<EGLDisplay>(gpu_->egl()->egl_display());
    auto create_sync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(gl_fns_[fn_create_sync]);
    auto destroy_sync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(gl_fns_[fn_destroy_sync]);
    auto dup_fence = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(gl_fns_[fn_dup_fence]);
    int fence = -1;
    if (native_fence_ && create_sync && destroy_sync && dup_fence) {
        EGLSyncKHR sync = create_sync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (sync != EGL_NO_SYNC_KHR) {
            // The fence fd exists only once the commands behind it are flushed.
            glFlush();
            fence = dup_fence(dpy, sync);
            destroy_sync(dpy, sync);
        }
    }
    if (fence < 0) {
        // No fence this frame: finish, then signal the point from the CPU.
        if (!warned_fence_) {
            warned_fence_ = true;
            log_swapchain_gl.warn("native fence export failed: glFinish and a CPU-signalled "
                                  "acquire point (reported once)");
        }
        glFinish();
        if (!ops_.signal || ops_.signal(ops_.user, s.timeline_handle, s.image.acquire_point) != 0)
            return lx::make_error(lx::drm_err::timeline_failed, "acquire point signal failed");
        return {};
    }
    const int r = ops_.import_sync_file(ops_.user, s.timeline_handle, s.image.acquire_point, fence);
    ::close(fence);
    if (r != 0)
        return lx::make_error(lx::drm_err::timeline_failed,
                              "native fence into the acquire point failed");
    ++stats_.native_fences;
    return {};
#else
    (void)s;
    return lx::not_implemented("lx::gfx::client_swapchain::present_gl");
#endif
}

lx::result<std::uint64_t> client_swapchain::wait_gl() {
#if defined(LUMEN_HAS_EGL)
    slot dummy{};
    if (auto current = acquire_gl(dummy); !current)
        return current.get_error();
    const std::uint64_t t0 = now_ns();
    glFinish();
    return now_ns() - t0;
#else
    return lx::not_implemented("lx::gfx::client_swapchain::wait_gl");
#endif
}

} // namespace lx::gfx
