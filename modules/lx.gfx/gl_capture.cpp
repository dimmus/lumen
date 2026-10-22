module;

// Bodies of `lx.gfx:gl_capture`. See gl_capture.cppm for the model.

#if defined(LUMEN_HAS_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <unistd.h>
#endif

module lx.gfx;

import std;

import lx.foundation;
import :damage;
import :gl_renderer;
import :gl_capture;

namespace lx::gfx {

namespace {

#if defined(LUMEN_HAS_EGL)
lx::error capture_error(const char* message) {
    return lx::make_error(lx::gl_err::draw_failed, message);
}

bool has_extension(const char* list, const char* name) {
    if (!list || !name)
        return false;
    const std::size_t n = std::strlen(name);
    for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n) {
        const bool start = p == list || p[-1] == ' ';
        const bool end = p[n] == '\0' || p[n] == ' ';
        if (start && end)
            return true;
    }
    return false;
}

/// The rects of `r` (full: one covering the target), clipped to the target.
unsigned rects_of(const damage_region& r, unsigned width, unsigned height, lx::rect2i* out) {
    const lx::rect2i bounds{0, 0, static_cast<int>(width), static_cast<int>(height)};
    if (r.full) {
        out[0] = bounds;
        return 1;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < r.count; ++i) {
        const lx::rect2i c = detail::damage_intersect(r.rects[i], bounds);
        if (c.width > 0 && c.height > 0)
            out[n++] = c;
    }
    return n;
}
#endif

} // namespace

gl_capture::~gl_capture() { shutdown(); }

lx::result<void> gl_capture::initialize(egl_device& device) {
#if defined(LUMEN_HAS_EGL)
    if (!device.valid())
        return lx::not_implemented("lx::gfx::gl_capture::initialize");
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    // "OpenGL ES 3.1 Mesa …": the major version after the prefix.
    int major = 0;
    if (version && std::strncmp(version, "OpenGL ES ", 10) == 0)
        major = std::atoi(version + 10);
    if (major < 3) {
        return lx::make_error(lx::gl_err::no_context,
                              "screen capture needs OpenGL ES 3.0 (blit, pixel-pack buffers)");
    }
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    read_bgra_ = has_extension(extensions, "GL_EXT_read_format_bgra");
    device_ = &device;
    ready_ = true;
    return {};
#else
    (void)device;
    return lx::not_implemented("lx::gfx::gl_capture::initialize");
#endif
}

void gl_capture::shutdown() {
    for (auto& t : targets_)
        destroy(t);
    for (auto& o : owners_)
        destroy(o);
    ready_ = false;
    device_ = nullptr;
}

void gl_capture::destroy(dmabuf_target& t) {
#if defined(LUMEN_HAS_EGL)
    if (t.framebuffer)
        glDeleteFramebuffers(1, &t.framebuffer);
    if (t.renderbuffer)
        glDeleteRenderbuffers(1, &t.renderbuffer);
    if (t.image && device_ && device_->egl_display()) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR")))
            destroy_image(static_cast<EGLDisplay>(device_->egl_display()),
                          static_cast<EGLImageKHR>(t.image));
    }
#endif
    t = {};
}

void gl_capture::destroy(owner_objects& o) {
#if defined(LUMEN_HAS_EGL)
    if (o.framebuffer)
        glDeleteFramebuffers(1, &o.framebuffer);
    if (o.renderbuffer)
        glDeleteRenderbuffers(1, &o.renderbuffer);
    if (o.pbo)
        glDeleteBuffers(1, &o.pbo);
    if (o.image && device_ && device_->egl_display()) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR")))
            destroy_image(static_cast<EGLDisplay>(device_->egl_display()),
                          static_cast<EGLImageKHR>(o.image));
    }
    if (o.bo)
        gbm_bo_destroy(static_cast<gbm_bo*>(o.bo));
#endif
    o = {};
}

gl_capture::owner_objects* gl_capture::owner(unsigned id, bool create) {
    for (auto& o : owners_)
        if (o.used && o.owner == id)
            return &o;
    if (!create)
        return nullptr;
    for (auto& o : owners_) {
        if (o.used)
            continue;
        o = {};
        o.used = true;
        o.owner = id;
        return &o;
    }
    return nullptr;
}

lx::result<unsigned> gl_capture::dmabuf_framebuffer(unsigned key, int fd, unsigned offset,
                                                    unsigned stride, unsigned width,
                                                    unsigned height, lx::fourcc format,
                                                    unsigned long long modifier) {
#if defined(LUMEN_HAS_EGL)
    if (!ready_ || !device_)
        return lx::not_implemented("lx::gfx::gl_capture::dmabuf_framebuffer");
    for (auto& t : targets_) {
        if (t.framebuffer && t.key == key && t.width == width && t.height == height) {
            t.used = ++clock_;
            return t.framebuffer;
        }
    }
    if (fd < 0)
        return lx::invalid_argument("capture dma-buf target without a plane");
    auto create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_to_renderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (!create_image || !image_to_renderbuffer)
        return capture_error("EGLImage renderbuffer entrypoints missing");
    auto dpy = static_cast<EGLDisplay>(device_->egl_display());
    const bool modifiers =
        has_extension(eglQueryString(dpy, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import_modifiers");
    constexpr unsigned long long k_mod_invalid = 0x00ffffffffffffffull;
    EGLint attrs[20];
    unsigned n = 0;
    attrs[n++] = EGL_WIDTH;
    attrs[n++] = static_cast<EGLint>(width);
    attrs[n++] = EGL_HEIGHT;
    attrs[n++] = static_cast<EGLint>(height);
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n++] = static_cast<EGLint>(format);
    attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[n++] = fd;
    attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[n++] = static_cast<EGLint>(offset);
    attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[n++] = static_cast<EGLint>(stride);
    if (modifiers && modifier != k_mod_invalid) {
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[n++] = static_cast<EGLint>(modifier & 0xFFFFFFFFull);
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[n++] = static_cast<EGLint>(modifier >> 32);
    }
    attrs[n++] = EGL_NONE;
    EGLImageKHR image =
        create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (image == EGL_NO_IMAGE_KHR)
        return capture_error("eglCreateImageKHR(capture dma-buf) failed");

    // A free entry, else the least recently used one (its buffer is imported again if it
    // comes back).
    dmabuf_target* slot = nullptr;
    for (auto& t : targets_)
        if (!t.framebuffer && !slot)
            slot = &t;
    if (!slot) {
        slot = &targets_[0];
        for (auto& t : targets_)
            if (t.used < slot->used)
                slot = &t;
        destroy(*slot);
    }
    for (auto& t : targets_)
        if (t.framebuffer && t.key == key)
            destroy(t); // the same key at another size (cannot happen: a buffer's size is fixed)
    slot->key = key;
    slot->image = image;
    slot->width = width;
    slot->height = height;
    slot->used = ++clock_;
    glGenRenderbuffers(1, &slot->renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, slot->renderbuffer);
    image_to_renderbuffer(GL_RENDERBUFFER, image);
    glGenFramebuffers(1, &slot->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              slot->renderbuffer);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        destroy(*slot);
        return capture_error("capture dma-buf is not renderable (framebuffer incomplete)");
    }
    return slot->framebuffer;
#else
    (void)key;
    (void)fd;
    (void)offset;
    (void)stride;
    (void)width;
    (void)height;
    (void)format;
    (void)modifier;
    return lx::not_implemented("lx::gfx::gl_capture::dmabuf_framebuffer");
#endif
}

void gl_capture::forget_dmabuf(unsigned key) {
    for (auto& t : targets_)
        if (t.framebuffer && t.key == key)
            destroy(t);
}

lx::result<unsigned> gl_capture::offscreen_framebuffer(unsigned id, unsigned width,
                                                       unsigned height) {
#if defined(LUMEN_HAS_EGL)
    if (!ready_)
        return lx::not_implemented("lx::gfx::gl_capture::offscreen_framebuffer");
    owner_objects* o = owner(id, true);
    if (!o)
        return lx::invalid_argument("capture offscreen table full");
    if (o->framebuffer && o->width == width && o->height == height)
        return o->framebuffer;
    // A new size: the old objects go (the pixel-pack buffer is sized on its next use).
    if (o->framebuffer)
        glDeleteFramebuffers(1, &o->framebuffer);
    if (o->renderbuffer)
        glDeleteRenderbuffers(1, &o->renderbuffer);
    o->framebuffer = o->renderbuffer = 0;
    if (o->image) {
        if (auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR")))
            destroy_image(static_cast<EGLDisplay>(device_->egl_display()),
                          static_cast<EGLImageKHR>(o->image));
        o->image = nullptr;
    }
    if (o->bo) {
        gbm_bo_destroy(static_cast<gbm_bo*>(o->bo));
        o->bo = nullptr;
    }
    glGenRenderbuffers(1, &o->renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, o->renderbuffer);
    // A LINEAR GBM buffer object as the storage, so the pixels can be mapped without GL.
    auto* gbm = static_cast<gbm_device*>(device_->gbm_device_handle());
    auto create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_to_renderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (gbm && create_image && image_to_renderbuffer) {
        const std::uint64_t linear = 0;
        gbm_bo* bo = gbm_bo_create_with_modifiers2(gbm, width, height, GBM_FORMAT_ARGB8888,
                                                   &linear, 1, GBM_BO_USE_RENDERING);
        if (!bo)
            bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888,
                               GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (bo) {
            EGLImageKHR image = create_image(static_cast<EGLDisplay>(device_->egl_display()),
                                             EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, nullptr);
            if (image != EGL_NO_IMAGE_KHR) {
                image_to_renderbuffer(GL_RENDERBUFFER, image);
                o->bo = bo;
                o->image = image;
            } else {
                gbm_bo_destroy(bo);
            }
        }
    }
    if (!o->bo)
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, static_cast<GLsizei>(width),
                              static_cast<GLsizei>(height));
    glGenFramebuffers(1, &o->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, o->framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              o->renderbuffer);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        const unsigned pbo = o->pbo;
        const unsigned pbo_bytes = o->pbo_bytes;
        o->pbo = 0;
        destroy(*o);
        o->used = true;
        o->owner = id;
        o->pbo = pbo;
        o->pbo_bytes = pbo_bytes;
        return capture_error("capture offscreen framebuffer incomplete");
    }
    o->width = width;
    o->height = height;
    return o->framebuffer;
#else
    (void)id;
    (void)width;
    (void)height;
    return lx::not_implemented("lx::gfx::gl_capture::offscreen_framebuffer");
#endif
}

void* gl_capture::offscreen_bo(unsigned id) {
    owner_objects* o = owner(id, false);
    return o ? o->bo : nullptr;
}

bool gl_capture::read_bo(void* bo, unsigned width, unsigned height, const damage_region& rects,
                         unsigned char* dst, unsigned dst_stride, int dx, int dy,
                         unsigned dst_width, unsigned dst_height, bool opaque) {
#if defined(LUMEN_HAS_EGL)
    if (!bo || !dst)
        return false;
    lx::rect2i r[k_damage_rects];
    unsigned n = rects_of(rects, width, height, r);
    unsigned kept = 0;
    for (unsigned i = 0; i < n; ++i) {
        const lx::rect2i c = detail::damage_intersect(
            r[i], {dx, dy, static_cast<int>(dst_width), static_cast<int>(dst_height)});
        if (c.width > 0 && c.height > 0)
            r[kept++] = c;
    }
    n = kept;
    if (n == 0)
        return true;
    // Map the bounding box of the rects only.
    int x0 = r[0].x, y0 = r[0].y, x1 = r[0].x + r[0].width, y1 = r[0].y + r[0].height;
    for (unsigned i = 1; i < n; ++i) {
        x0 = r[i].x < x0 ? r[i].x : x0;
        y0 = r[i].y < y0 ? r[i].y : y0;
        x1 = r[i].x + r[i].width > x1 ? r[i].x + r[i].width : x1;
        y1 = r[i].y + r[i].height > y1 ? r[i].y + r[i].height : y1;
    }
    std::uint32_t stride = 0;
    void* data = nullptr;
    auto* mapped = static_cast<const unsigned char*>(
        gbm_bo_map(static_cast<gbm_bo*>(bo), static_cast<std::uint32_t>(x0),
                   static_cast<std::uint32_t>(y0), static_cast<std::uint32_t>(x1 - x0),
                   static_cast<std::uint32_t>(y1 - y0), GBM_BO_TRANSFER_READ, &stride, &data));
    if (!mapped)
        return false;
    for (unsigned i = 0; i < n; ++i) {
        const std::size_t bytes = static_cast<std::size_t>(r[i].width) * 4u;
        for (int y = r[i].y; y < r[i].y + r[i].height; ++y) {
            const unsigned char* s = mapped + static_cast<std::size_t>(y - y0) * stride +
                                     static_cast<std::size_t>(r[i].x - x0) * 4u;
            unsigned char* d = dst + static_cast<std::size_t>(y - dy) * dst_stride +
                               static_cast<std::size_t>(r[i].x - dx) * 4u;
            std::memcpy(d, s, bytes);
            if (opaque)
                for (std::size_t k = 3; k < bytes; k += 4)
                    d[k] = 0xFF;
        }
    }
    gbm_bo_unmap(static_cast<gbm_bo*>(bo), data);
    return true;
#else
    (void)bo;
    (void)width;
    (void)height;
    (void)rects;
    (void)dst;
    (void)dst_stride;
    (void)dx;
    (void)dy;
    (void)dst_width;
    (void)dst_height;
    (void)opaque;
    return false;
#endif
}

void gl_capture::forget_owner(unsigned id) {
    if (owner_objects* o = owner(id, false))
        destroy(*o);
}

lx::result<void> gl_capture::blit(unsigned src, unsigned dst, unsigned width, unsigned height,
                                  const damage_region& rects, int dx, int dy) {
#if defined(LUMEN_HAS_EGL)
    if (!ready_ || src == 0 || dst == 0)
        return lx::not_implemented("lx::gfx::gl_capture::blit");
    lx::rect2i r[k_damage_rects];
    const unsigned n = rects_of(rects, width, height, r);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    // Both have row 0 on top (memory row 0), so a rect lands where it was (less the crop's
    // origin): no flip.
    for (unsigned i = 0; i < n; ++i)
        glBlitFramebuffer(r[i].x, r[i].y, r[i].x + r[i].width, r[i].y + r[i].height, r[i].x - dx,
                          r[i].y - dy, r[i].x - dx + r[i].width, r[i].y - dy + r[i].height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return capture_error("glBlitFramebuffer (capture) failed");
    return {};
#else
    (void)src;
    (void)dst;
    (void)width;
    (void)height;
    (void)rects;
    (void)dx;
    (void)dy;
    return lx::not_implemented("lx::gfx::gl_capture::blit");
#endif
}

lx::result<void> gl_capture::clear(unsigned dst, unsigned width, unsigned height,
                                   lx::color colour) {
#if defined(LUMEN_HAS_EGL)
    if (!ready_ || dst == 0)
        return lx::not_implemented("lx::gfx::gl_capture::clear");
    glBindFramebuffer(GL_FRAMEBUFFER, dst);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_SCISSOR_TEST);
    glClearColor(colour.r, colour.g, colour.b, colour.a);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return capture_error("glClear (capture curtain) failed");
    return {};
#else
    (void)dst;
    (void)width;
    (void)height;
    (void)colour;
    return lx::not_implemented("lx::gfx::gl_capture::clear");
#endif
}

lx::result<void> gl_capture::read_async(unsigned id, unsigned src, unsigned width,
                                        unsigned height, const damage_region& rects) {
#if defined(LUMEN_HAS_EGL)
    if (!ready_ || src == 0)
        return lx::not_implemented("lx::gfx::gl_capture::read_async");
    owner_objects* o = owner(id, true);
    if (!o)
        return lx::invalid_argument("capture read-back table full");
    const unsigned bytes = width * height * 4u;
    if (!o->pbo) {
        glGenBuffers(1, &o->pbo);
        o->pbo_bytes = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, o->pbo);
    if (o->pbo_bytes != bytes) {
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        o->pbo_bytes = bytes;
    }
    lx::rect2i r[k_damage_rects];
    const unsigned n = rects_of(rects, width, height, r);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(width));
    const GLenum format = read_bgra_ ? GL_BGRA_EXT : GL_RGBA;
    for (unsigned i = 0; i < n; ++i) {
        // Rows land at their place in a width × 4-byte image: the copy out finds them there.
        const std::size_t offset =
            (static_cast<std::size_t>(r[i].y) * width + static_cast<std::size_t>(r[i].x)) * 4u;
        glReadPixels(r[i].x, r[i].y, r[i].width, r[i].height, format, GL_UNSIGNED_BYTE,
                     reinterpret_cast<void*>(offset));
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return capture_error("glReadPixels into a pixel-pack buffer failed");
    return {};
#else
    (void)id;
    (void)src;
    (void)width;
    (void)height;
    (void)rects;
    return lx::not_implemented("lx::gfx::gl_capture::read_async");
#endif
}

lx::result<void> gl_capture::finish_read(unsigned id, unsigned char* dst, unsigned dst_stride,
                                         unsigned width, unsigned height,
                                         const damage_region& rects, int dx, int dy,
                                         unsigned dst_width, unsigned dst_height) {
#if defined(LUMEN_HAS_EGL)
    owner_objects* o = owner(id, false);
    if (!ready_ || !o || !o->pbo || !dst || o->pbo_bytes != width * height * 4u)
        return lx::invalid_argument("capture read-back finished without a read");
    if (dst_width == 0 || dst_height == 0) {
        dst_width = width;
        dst_height = height;
    }
    lx::rect2i r[k_damage_rects];
    unsigned n = rects_of(rects, width, height, r);
    // Only what lands in the destination (a crop) is copied.
    unsigned kept = 0;
    for (unsigned i = 0; i < n; ++i) {
        const lx::rect2i c = detail::damage_intersect(
            r[i], {dx, dy, static_cast<int>(dst_width), static_cast<int>(dst_height)});
        if (c.width > 0 && c.height > 0)
            r[kept++] = c;
    }
    n = kept;
    if (n == 0)
        return {};
    // Map only the rows the rects span.
    int y0 = r[0].y;
    int y1 = r[0].y + r[0].height;
    for (unsigned i = 1; i < n; ++i) {
        if (r[i].y < y0)
            y0 = r[i].y;
        if (r[i].y + r[i].height > y1)
            y1 = r[i].y + r[i].height;
    }
    const std::size_t row = static_cast<std::size_t>(width) * 4u;
    const std::size_t first = static_cast<std::size_t>(y0) * row;
    const std::size_t length = static_cast<std::size_t>(y1 - y0) * row;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, o->pbo);
    auto* mapped = static_cast<const unsigned char*>(glMapBufferRange(
        GL_PIXEL_PACK_BUFFER, static_cast<GLintptr>(first), static_cast<GLsizeiptr>(length),
        GL_MAP_READ_BIT));
    if (!mapped) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return capture_error("glMapBufferRange (capture read-back) failed");
    }
    for (unsigned i = 0; i < n; ++i) {
        const std::size_t bytes = static_cast<std::size_t>(r[i].width) * 4u;
        for (int y = r[i].y; y < r[i].y + r[i].height; ++y) {
            const unsigned char* s =
                mapped + static_cast<std::size_t>(y - y0) * row + static_cast<std::size_t>(r[i].x) * 4u;
            unsigned char* d = dst + static_cast<std::size_t>(y - dy) * dst_stride +
                               static_cast<std::size_t>(r[i].x - dx) * 4u;
            if (read_bgra_) {
                std::memcpy(d, s, bytes);
            } else {
                for (std::size_t k = 0; k < bytes; k += 4) {
                    d[k] = s[k + 2];
                    d[k + 1] = s[k + 1];
                    d[k + 2] = s[k];
                    d[k + 3] = s[k + 3];
                }
            }
        }
    }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return {};
#else
    (void)id;
    (void)dst;
    (void)dst_stride;
    (void)width;
    (void)height;
    (void)rects;
    (void)dx;
    (void)dy;
    (void)dst_width;
    (void)dst_height;
    return lx::not_implemented("lx::gfx::gl_capture::finish_read");
#endif
}

int gl_capture::fence() {
#if defined(LUMEN_HAS_EGL)
    if (!ready_ || !device_)
        return -1;
    if (device_->supports_native_fence()) {
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
                glFlush();
                const EGLint fd = dup_fence(dpy, sync);
                destroy_sync(dpy, sync);
                if (fd != EGL_NO_NATIVE_FENCE_FD_ANDROID)
                    return fd;
            }
        }
    }
    glFlush();
    return -1;
#else
    return -1;
#endif
}

} // namespace lx::gfx
