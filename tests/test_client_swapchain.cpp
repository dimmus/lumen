// lx.gfx:swapchain (ADR-010 §4.10) against a real compositor, in-process (the harness of
// test_wayland_client: lx::compositor::compositor on a private socket, one virtual output,
// CPU composite, a dispatch pump thread, frames ticked by the test):
//
//   - the pure parts: format choice from feedback tranches, the tier choice and its refusals;
//   - what the headless compositor offers (dma-buf feedback) and which tier a window gets;
//   - per tier (cpu; gl on the render node; vulkan forced on lavapipe, labelled software):
//     three frames of known golden primitive streams presented through wl_buffers, and the
//     buffer the compositor received read back and held to G10 against the committed CPU
//     references (dma-buf tiers: the plane fd, offsets, strides and modifier the compositor
//     registered, imported into a GL context of the test's own or mapped; shm: the window in
//     an ext-image-copy-capture output capture, i.e. the compositor's own composite);
//   - the ring: buffer age 0,0,0 then 3 (0,0 then 2 for shm) under round-robin release,
//     acquire with every image busy returns none at once, resize reallocates once per size and
//     frees old images only after their release, a 30-size storm;
//   - explicit sync on GL: the native fence goes into the acquire point and the image stays
//     busy until its release point signals (a fake kernel timeline — vmwgfx has no timeline
//     syncobjs and the headless compositor advertises no wp_linux_drm_syncobj_manager_v1, so
//     the wire half is skipped with that reason); the implicit fallback forced by option;
//   - allocations per steady present (operator new on the client thread: must be 0; malloc /
//     free from libwayland and the driver reported) and the per-frame present cost per tier
//     at 640×480 (acquire → draw → present → attach/damage/commit), p50/p95.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE) &&                          \
    defined(LUMEN_PROTOCOL_XDG_SHELL) && defined(LUMEN_PROTOCOL_EXT_IMAGE_CAPTURE_SOURCE_V1) && \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_COPY_CAPTURE_V1)
#include <wayland-client.h>
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#define LUMEN_TEST_SWAPCHAIN 1
#endif
#if defined(LUMEN_HAS_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#endif
#include <stdint.h>

#include "ui_golden.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.compositor;
import lx.wayland.client;
import lx.gfx;
import lx.scene;
import lx.ui;

// ── allocation accounting (client thread only, while `g_counting`) ──

#if defined(__SANITIZE_ADDRESS__)
#define LUMEN_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LUMEN_TEST_SANITIZED 1
#endif
#endif

namespace {
__attribute__((tls_model("initial-exec"))) thread_local bool g_counting = false;
std::atomic<std::uint64_t> g_new_calls{0};
std::atomic<std::uint64_t> g_malloc_calls{0};
std::atomic<std::uint64_t> g_free_calls{0};
} // namespace

#if !defined(LUMEN_TEST_SANITIZED)
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void __libc_free(void*);

void* malloc(size_t n) noexcept {
    if (g_counting)
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    return __libc_malloc(n);
}
void* calloc(size_t n, size_t s) noexcept {
    if (g_counting)
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    return __libc_calloc(n, s);
}
void* realloc(void* p, size_t n) noexcept {
    if (g_counting)
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    return __libc_realloc(p, n);
}
void free(void* p) noexcept {
    if (g_counting && p)
        g_free_calls.fetch_add(1, std::memory_order_relaxed);
    __libc_free(p);
}
}

namespace {
void* counted_new(std::size_t n) noexcept {
    if (g_counting)
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* counted_aligned_new(std::size_t n, std::align_val_t al) noexcept {
    if (g_counting)
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
    const auto a = static_cast<std::size_t>(al);
    const std::size_t rounded = (n + a - 1) / a * a;
    return std::aligned_alloc(a, rounded ? rounded : a);
}
} // namespace

void* operator new(std::size_t n) {
    if (void* p = counted_new(n))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    if (void* p = counted_new(n))
        return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = counted_aligned_new(n, a))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) {
    if (void* p = counted_aligned_new(n, a))
        return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned_new(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned_new(n, a);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

#if !defined(LUMEN_TEST_SWAPCHAIN)

int main() {
    std::printf("SKIP: needs libwayland-client and the protocol glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-client-swapchain";
constexpr unsigned k_w = 640;
constexpr unsigned k_h = 480;

int g_failed = 0;
int g_ran = 0;
int g_skipped = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 0;
std::vector<std::string> g_measurements;

void check(const char* name, bool ok, const std::string& detail) {
    ++g_ran;
    std::printf("  %s  %s — %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

void skip(const char* name, const std::string& why) {
    ++g_skipped;
    std::printf("  SKIP  %s — %s\n", name, why.c_str());
}

void measure(std::string line) {
    std::printf("  MEASURE  %s\n", line.c_str());
    g_measurements.push_back(std::move(line));
}

std::uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

/// One compositor frame on this thread: UI tick, render tick, posted UI work. Never counted.
void tick() {
    const bool counting = g_counting;
    g_counting = false;
    {
        std::lock_guard<std::mutex> hold(*g_lock);
        lx::scheduler::frame_tick t{};
        t.timestamp_ns = static_cast<long long>(now_ns());
        t.delta_seconds = 1.0 / 30.0;
        t.frame_index = static_cast<unsigned>(++g_frame);
        g_comp->tick_ui(t);
        g_comp->wayland().flush();
        (void)g_comp->render_and_wait();
        g_comp->run_posted_ui_tasks();
        g_comp->wayland().flush();
    }
    g_counting = counting;
}

template <typename F>
bool pump(lx::wayland::display& d, F done, int rounds = 100) {
    for (int i = 0; i < rounds && !done(); ++i) {
        if (!d.dispatch(10))
            return false;
    }
    return done();
}

lx::compositor::toplevel_record record(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::compositor::toplevel_record out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id.id()) {
            out.id = r.id;
            out.surface = r.surface;
            out.geometry = r.geometry;
            out.mapped = r.mapped;
        }
    });
    return out;
}

struct percentiles {
    double p50 = 0;
    double p95 = 0;
    double max = 0;
};

percentiles stats_of(std::vector<double> v) {
    percentiles p{};
    if (v.empty())
        return p;
    std::sort(v.begin(), v.end());
    p.p50 = v[v.size() / 2];
    p.p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
    p.max = v.back();
    return p;
}

double load_average() {
    double l = -1;
    if (FILE* f = std::fopen("/proc/loadavg", "r")) {
        if (std::fscanf(f, "%lf", &l) != 1)
            l = -1;
        std::fclose(f);
    }
    return l;
}

bool same_file(int a, int b) {
    struct stat sa {}, sb {};
    return a >= 0 && b >= 0 && ::fstat(a, &sa) == 0 && ::fstat(b, &sb) == 0 &&
           sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev;
}

// ── the window: a surface, a toplevel and one wl_buffer per swapchain image ──

struct window final : lx::wayland::toplevel_handler,
                      lx::wayland::buffer_handler,
                      lx::gfx::swapchain_listener {
    lx::wayland::display* d = nullptr;
    lx::gfx::client_swapchain* chain = nullptr;
    lx::wayland::surface surface;
    lx::wayland::toplevel top;
    unsigned configures = 0;
    lx::wayland::toplevel_configure last{};
    struct entry {
        std::uint32_t id = 0;
        std::unique_ptr<lx::wayland::buffer> buffer;
    };
    std::vector<entry> buffers;
    unsigned created = 0;
    unsigned destroyed = 0;
    unsigned releases = 0;
    unsigned failed = 0;
    bool make_buffers = true;

    void on_configure(lx::wayland::toplevel&, const lx::wayland::toplevel_configure& c) override {
        ++configures;
        last = c;
    }
    void on_release(lx::wayland::buffer& b) override {
        ++releases;
        for (entry& e : buffers)
            if (e.buffer.get() == &b && chain)
                chain->release(e.id);
    }
    void on_failed(lx::wayland::buffer&) override { ++failed; }

    void on_image_created(const lx::gfx::swapchain_image& image) override {
        ++created;
        if (!make_buffers)
            return;
        entry e{};
        e.id = image.id;
        e.buffer = std::make_unique<lx::wayland::buffer>();
        const lx::size2i size{static_cast<int>(image.width), static_cast<int>(image.height)};
        bool ok = false;
        if (image.pixels.pixels) {
            lx::wayland::shm_pool pool;
            ok = pool.create(*d, image.planes[0].fd, image.pool_bytes).ok() &&
                 e.buffer
                     ->create_shm(pool, image.planes[0].offset, size,
                                  static_cast<std::int32_t>(image.planes[0].stride),
                                  image.fourcc == lx::gfx::k_fourcc_xrgb8888 ? 1u : 0u)
                     .ok();
            pool.destroy();
        } else {
            lx::wayland::dmabuf_plane planes[4]{};
            for (unsigned p = 0; p < image.plane_count; ++p)
                planes[p] = {image.planes[p].fd, image.planes[p].offset, image.planes[p].stride};
            ok = e.buffer
                     ->create_dmabuf(*d, std::span{planes, image.plane_count}, size, image.fourcc,
                                     image.modifier)
                     .ok();
        }
        if (!ok)
            ++failed;
        e.buffer->set_handler(this);
        buffers.push_back(std::move(e));
    }
    void on_image_destroyed(std::uint32_t id) override {
        ++destroyed;
        std::erase_if(buffers, [id](const entry& e) { return e.id == id; });
    }
    lx::wayland::buffer* buffer_of(std::uint32_t id) {
        for (entry& e : buffers)
            if (e.id == id)
                return e.buffer.get();
        return nullptr;
    }

    bool map(lx::wayland::display& disp, const char* app_id) {
        d = &disp;
        if (!surface.create(disp).ok())
            return false;
        if (!top.create(disp, surface,
                        {.title = "swapchain", .app_id = app_id, .server_side_decorations = false})
                 .ok())
            return false;
        top.set_handler(this);
        surface.commit();
        return pump(disp, [&] { return configures >= 1; });
    }
    /// Attach + damage + commit of `image`.
    void commit(const lx::gfx::swapchain_image& image, const lx::gfx::damage_region& dmg) {
        if (top.acked_serial() != last.serial)
            top.ack_configure(last.serial);
        surface.attach(buffer_of(image.id));
        if (dmg.full || dmg.count == 0) {
            surface.damage_buffer({0, 0, static_cast<int>(image.width),
                                   static_cast<int>(image.height)});
        } else {
            for (unsigned i = 0; i < dmg.count; ++i)
                surface.damage_buffer(dmg.rects[i]);
        }
        (void)surface.commit();
        (void)d->flush();
    }
    void destroy() {
        buffers.clear();
        top.destroy();
        surface.destroy();
    }
};

// ── the tiers' renderers, behind one draw call ──

struct renderers {
    lx::gfx::ui_cpu_renderer cpu{};
    std::unique_ptr<lx::gfx::ui_gl_renderer> gl{};
    std::unique_ptr<lx::gfx::ui_vk_renderer> vk{};
    std::uint64_t slot_wait_ns = 0; ///< gl: time `draw` waited for its ring slot's fence

    lx::result<void> init(lx::gfx::client_gpu& gpu) {
        if (gpu.tier() == lx::gfx::client_tier::gl) {
            gl = std::make_unique<lx::gfx::ui_gl_renderer>();
            return gl->initialize();
        }
        if (gpu.tier() == lx::gfx::client_tier::vulkan) {
            vk = std::make_unique<lx::gfx::ui_vk_renderer>();
            lx::gfx::ui_vk_options o{};
            o.format = lx::gfx::k_ui_vk_bgra8_unorm;
            return vk->initialize(gpu.vulkan_context(), o);
        }
        return {};
    }
    bool draw(lx::gfx::swapchain_image& image, const lx::gfx::ui_stream& stream,
              lx::gfx::atlas* atlas, const lx::gfx::damage_region& dmg,
              std::optional<lx::color> clear) {
        if (gl) {
            auto drawn = gl->draw(image.gl_framebuffer, image.width, image.height, stream, atlas,
                                  dmg, clear);
            if (drawn)
                slot_wait_ns += drawn->slot_wait_ns;
            return static_cast<bool>(drawn);
        }
        if (vk)
            return static_cast<bool>(vk->draw(image.vk, stream, atlas, dmg, clear));
        return static_cast<bool>(cpu.draw(image.pixels, stream, atlas, dmg, clear));
    }
    void shutdown() {
        if (vk)
            vk->wait_idle();
        vk.reset();
        gl.reset();
    }
};

// ── reading back what the compositor received ──

/// The dma-buf the compositor registered for `app_id`'s surface: its plane fds dup'd.
struct received {
    lx::gfx::dmabuf_desc desc{};
    bool ok = false;
};

received compositor_dmabuf(const char* app_id) {
    const auto rec = record(app_id);
    received out{};
    std::lock_guard<std::mutex> hold(*g_lock);
    const lx::gfx::dmabuf_desc* d = g_comp->surfaces().current_dmabuf(rec.surface);
    if (!d)
        return out;
    out.desc.width = d->width;
    out.desc.height = d->height;
    out.desc.format = d->format;
    out.desc.plane_count = d->plane_count;
    for (unsigned p = 0; p < d->plane_count; ++p) {
        out.desc.planes[p].fd.reset(::fcntl(d->planes[p].fd.get(), F_DUPFD_CLOEXEC, 0));
        out.desc.planes[p].offset = d->planes[p].offset;
        out.desc.planes[p].stride = d->planes[p].stride;
        out.desc.planes[p].modifier = d->planes[p].modifier;
    }
    out.ok = true;
    return out;
}

#if defined(LUMEN_HAS_EGL)
/// A GL context of the test's own on the render node: imports a dma-buf the way the GL
/// compositor does (EGL_EXT_image_dma_buf_import with the modifier) and reads it back.
struct gl_reader {
    lx::unique_fd fd{};
    lx::gfx::egl_device egl{};
    bool ready = false;

    bool init() {
        auto node = lx::gfx::open_render_node(0);
        if (!node)
            return false;
        fd = std::move(node.value().fd);
        auto dev = lx::gfx::egl_device::create(fd.get());
        if (!dev)
            return false;
        egl = std::move(dev).value();
        ready = true;
        return true;
    }
    bool read(const lx::gfx::dmabuf_desc& d, std::vector<std::uint32_t>& out) {
        if (!ready || !egl.make_current())
            return false;
        auto dpy = static_cast<EGLDisplay>(egl.egl_display());
        auto create_image =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        auto destroy_image =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        auto to_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!create_image || !destroy_image || !to_texture)
            return false;
        const std::uint64_t mod = d.planes[0].modifier;
        EGLint attrs[] = {EGL_WIDTH,
                          static_cast<EGLint>(d.width),
                          EGL_HEIGHT,
                          static_cast<EGLint>(d.height),
                          EGL_LINUX_DRM_FOURCC_EXT,
                          static_cast<EGLint>(d.format),
                          EGL_DMA_BUF_PLANE0_FD_EXT,
                          d.planes[0].fd.get(),
                          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                          static_cast<EGLint>(d.planes[0].offset),
                          EGL_DMA_BUF_PLANE0_PITCH_EXT,
                          static_cast<EGLint>(d.planes[0].stride),
                          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                          static_cast<EGLint>(mod & 0xffffffffu),
                          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                          static_cast<EGLint>(mod >> 32),
                          EGL_NONE};
        if (mod == lx::gfx::k_modifier_invalid)
            attrs[12] = EGL_NONE;
        EGLImageKHR image = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (image == EGL_NO_IMAGE_KHR)
            return false;
        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        to_texture(GL_TEXTURE_2D, image);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(d.width) * d.height * 4u);
        if (complete)
            glReadPixels(0, 0, static_cast<GLsizei>(d.width), static_cast<GLsizei>(d.height),
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        destroy_image(dpy, image);
        if (!complete || glGetError() != GL_NO_ERROR)
            return false;
        out.resize(static_cast<std::size_t>(d.width) * d.height);
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = 0xFF000000u | (std::uint32_t{rgba[i * 4]} << 16) |
                     (std::uint32_t{rgba[i * 4 + 1]} << 8) | rgba[i * 4 + 2];
        return true;
    }
};
#endif

/// A linear dma-buf mapped (udmabuf / memfd-backed exporters such as lavapipe), bracketed by
/// DMA_BUF_IOCTL_SYNC so the reader waits for the writer's fences.
bool map_read(const lx::gfx::dmabuf_desc& d, std::vector<std::uint32_t>& out) {
    if (d.planes[0].modifier != lx::gfx::k_modifier_linear)
        return false;
    const int fd = d.planes[0].fd.get();
    const std::size_t bytes = static_cast<std::size_t>(d.planes[0].offset) +
                              static_cast<std::size_t>(d.planes[0].stride) * d.height;
    void* map = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return false;
    dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    out.resize(static_cast<std::size_t>(d.width) * d.height);
    const auto* base = static_cast<const std::uint8_t*>(map) + d.planes[0].offset;
    for (unsigned y = 0; y < d.height; ++y)
        std::memcpy(out.data() + static_cast<std::size_t>(y) * d.width,
                    base + static_cast<std::size_t>(y) * d.planes[0].stride, d.width * 4u);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    ::munmap(map, bytes);
    for (auto& p : out)
        p |= 0xFF000000u;
    return true;
}

// ── output capture (ext-image-copy-capture-v1) for the shm tier ──

struct capture_client {
    wl_display* display = nullptr;
    wl_shm* shm = nullptr;
    wl_output* output = nullptr;
    ext_output_image_capture_source_manager_v1* sources = nullptr;
    ext_image_copy_capture_manager_v1* copy = nullptr;
    int buffer_w = 0;
    int buffer_h = 0;
    unsigned dones = 0;
    bool ready = false;
    bool failed = false;
};

const wl_registry_listener k_capture_registry = {
    [](void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t) {
        auto* c = static_cast<capture_client*>(data);
        const auto is = [&](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
        if (is(wl_shm_interface))
            c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
        else if (is(wl_output_interface) && !c->output)
            c->output = static_cast<wl_output*>(wl_registry_bind(r, name, &wl_output_interface, 1));
        else if (is(ext_output_image_capture_source_manager_v1_interface))
            c->sources = static_cast<ext_output_image_capture_source_manager_v1*>(wl_registry_bind(
                r, name, &ext_output_image_capture_source_manager_v1_interface, 1));
        else if (is(ext_image_copy_capture_manager_v1_interface))
            c->copy = static_cast<ext_image_copy_capture_manager_v1*>(
                wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1));
    },
    [](void*, wl_registry*, uint32_t) {}};

const ext_image_copy_capture_session_v1_listener k_session_listener = {
    .buffer_size =
        [](void* d, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
            static_cast<capture_client*>(d)->buffer_w = static_cast<int>(w);
            static_cast<capture_client*>(d)->buffer_h = static_cast<int>(h);
        },
    .shm_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t) {},
    .dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
    .dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
    .done = [](void* d, ext_image_copy_capture_session_v1*) { ++static_cast<capture_client*>(d)->dones; },
    .stopped = [](void*, ext_image_copy_capture_session_v1*) {},
};

const ext_image_copy_capture_frame_v1_listener k_frame_listener = {
    .transform = [](void*, ext_image_copy_capture_frame_v1*, uint32_t) {},
    .damage = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
    .presentation_time = [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .ready = [](void* d, ext_image_copy_capture_frame_v1*) { static_cast<capture_client*>(d)->ready = true; },
    .failed = [](void* d, ext_image_copy_capture_frame_v1*,
                 uint32_t) { static_cast<capture_client*>(d)->failed = true; },
};

/// The output composited by the compositor, captured into shm (XRGB8888 words).
bool capture_output(std::vector<std::uint32_t>& out, int& w, int& h) {
    capture_client c{};
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    wl_registry* reg = wl_display_get_registry(c.display);
    wl_registry_add_listener(reg, &k_capture_registry, &c);
    (void)wl_display_roundtrip(c.display);
    bool ok = c.shm && c.output && c.sources && c.copy;
    if (ok) {
        auto* source = ext_output_image_capture_source_manager_v1_create_source(c.sources, c.output);
        auto* session = ext_image_copy_capture_manager_v1_create_session(c.copy, source, 0);
        ext_image_copy_capture_session_v1_add_listener(session, &k_session_listener, &c);
        for (int i = 0; i < 50 && c.dones == 0; ++i)
            (void)wl_display_roundtrip(c.display);
        w = c.buffer_w;
        h = c.buffer_h;
        const std::size_t bytes = static_cast<std::size_t>(w) * h * 4u;
        const int fd = ::memfd_create("lumen-test-capture", MFD_CLOEXEC);
        ok = c.dones > 0 && fd >= 0 && ::ftruncate(fd, static_cast<off_t>(bytes)) == 0;
        void* map = ok ? ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        ok = ok && map != MAP_FAILED;
        if (ok) {
            wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, static_cast<int32_t>(bytes));
            wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
            auto* frame = ext_image_copy_capture_session_v1_create_frame(session);
            ext_image_copy_capture_frame_v1_add_listener(frame, &k_frame_listener, &c);
            ext_image_copy_capture_frame_v1_attach_buffer(frame, buf);
            ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, w, h);
            ext_image_copy_capture_frame_v1_capture(frame);
            (void)wl_display_flush(c.display);
            for (int i = 0; i < 40 && !c.ready && !c.failed; ++i) {
                tick();
                (void)wl_display_roundtrip(c.display);
            }
            ok = c.ready;
            if (ok) {
                out.assign(static_cast<const std::uint32_t*>(map),
                           static_cast<const std::uint32_t*>(map) + static_cast<std::size_t>(w) * h);
            }
            ext_image_copy_capture_frame_v1_destroy(frame);
            wl_buffer_destroy(buf);
            ::munmap(map, bytes);
        }
        if (fd >= 0)
            ::close(fd);
        ext_image_copy_capture_session_v1_destroy(session);
        ext_image_capture_source_v1_destroy(source);
    }
    if (c.copy)
        ext_image_copy_capture_manager_v1_destroy(c.copy);
    if (c.sources)
        ext_output_image_capture_source_manager_v1_destroy(c.sources);
    if (c.output)
        wl_output_destroy(c.output);
    if (c.shm)
        wl_shm_destroy(c.shm);
    wl_registry_destroy(reg);
    wl_display_disconnect(c.display);
    return ok;
}

// ── the cases ──

/// The three frames of every tier: golden cases with sprites, shadows and borders.
const ui_golden::golden_case* golden_case_named(const char* name) {
    for (const ui_golden::golden_case& c : ui_golden::cases())
        if (std::strcmp(c.name, name) == 0)
            return &c;
    return nullptr;
}

constexpr const char* k_frames[3] = {"quad_borders", "shadow_stacks", "sprites"};

lx::gfx::client_feedback feedback_of(const lx::wayland::dmabuf_feedback_snapshot& s,
                                     std::vector<lx::gfx::client_format>& storage,
                                     std::vector<lx::gfx::client_tranche>& tranches) {
    storage.clear();
    tranches.clear();
    std::size_t total = 0;
    for (const auto& t : s.list())
        total += t.count;
    storage.reserve(total);
    for (const auto& t : s.list()) {
        const std::size_t first = storage.size();
        for (const auto& f : t.entries())
            storage.push_back({f.format, f.modifier});
        tranches.push_back({t.target_device, t.scanout(),
                            std::span<const lx::gfx::client_format>{storage.data() + first, t.count}});
    }
    return {s.main_device, tranches};
}

void pure_cases() {
    using namespace lx::gfx;
    const client_format main_formats[] = {{k_fourcc_argb8888, 0x100000000000001ull},
                                          {k_fourcc_argb8888, k_modifier_linear},
                                          {k_fourcc_xrgb8888, k_modifier_linear},
                                          {k_fourcc_xrgb8888, 0x100000000000001ull}};
    const client_format scanout_formats[] = {{k_fourcc_xrgb8888, k_modifier_linear}};
    const client_format other_formats[] = {{k_fourcc_xrgb8888, 0x200000000000001ull}};
    const client_tranche tranches[] = {
        {0xfff01, false, other_formats}, // another device: skipped
        {0xfff00, false, main_formats},
        {0xfff00, true, scanout_formats},
    };
    const client_feedback fb{0xfff00, tranches};
    auto opaque = choose_client_format(fb, {.opaque = true});
    check("format: opaque takes the scanout tranche's XRGB8888 set first",
          opaque && opaque->fourcc == k_fourcc_xrgb8888 && opaque->scanout &&
              opaque->modifier_count == 1 && opaque->modifiers[0] == k_modifier_linear,
          opaque ? std::format("{:#x} scanout {} mods {}", opaque->fourcc, opaque->scanout,
                               opaque->modifier_count)
                 : lx::format_error(opaque.get_error()));
    auto no_scanout = choose_client_format(fb, {.opaque = true, .prefer_scanout = false});
    check("format: without scanout preference the main tranche, every modifier for the fourcc",
          no_scanout && !no_scanout->scanout && no_scanout->modifier_count == 2 &&
              no_scanout->has(0x100000000000001ull) && no_scanout->has(k_modifier_linear),
          no_scanout ? std::format("mods {}", no_scanout->modifier_count) : "error");
    auto alpha = choose_client_format(fb, {.opaque = false});
    check("format: translucent windows take ARGB8888 (the scanout tranche has none)",
          alpha && alpha->fourcc == k_fourcc_argb8888 && !alpha->scanout &&
              alpha->modifier_count == 2,
          alpha ? std::format("{:#x} mods {}", alpha->fourcc, alpha->modifier_count) : "error");
    auto other = choose_client_format(fb, {.opaque = true, .prefer_scanout = false}, 0xfff01);
    check("format: a device filter takes that device's tranche only",
          other && other->device == 0xfff01 && other->modifiers[0] == 0x200000000000001ull, "");
    const client_tranche none[] = {{0xfff00, false, other_formats}};
    auto missing = choose_client_format({0xfff01, none}, {.opaque = false});
    check("format: no tranche for the device is drm_err::no_format (tier-neutral)",
          !missing && missing.get_error().domain == lx::error_domain::drm &&
              missing.get_error().code == static_cast<int>(lx::drm_err::no_format),
          "");
    check("tier names parse (LUMEN_UI_BACKEND)",
          parse_client_tier("vulkan") == client_tier::vulkan &&
              parse_client_tier("gl") == client_tier::gl &&
              parse_client_tier("cpu") == client_tier::cpu &&
              parse_client_tier("bogus") == client_tier::none,
          "");
}

// A fake kernel timeline: syncobjs are table entries, points are values; `import_sync_file`
// records the fence it was given (and checks it is a real sync_file: it polls).
struct fake_timeline {
    struct obj {
        bool used = false;
        std::uint64_t signalled = 0;
        std::uint64_t last_import = 0;
        unsigned imports = 0;
        unsigned valid_fences = 0;
        int eventfd = -1;
        std::uint64_t wait_point = 0;
    };
    obj objs[16]{};
    unsigned creates = 0;
    unsigned eventfds = 0;

    static fake_timeline* of(void* u) { return static_cast<fake_timeline*>(u); }
    lx::gfx::drm_timeline_ops ops() {
        lx::gfx::drm_timeline_ops o{};
        o.user = this;
        o.create = [](void* u, unsigned* h) {
            auto* t = of(u);
            for (unsigned i = 1; i < 16; ++i)
                if (!t->objs[i].used) {
                    t->objs[i] = {};
                    t->objs[i].used = true;
                    *h = i;
                    ++t->creates;
                    return 0;
                }
            return -ENOSPC;
        };
        o.handle_to_fd = [](void*, unsigned, int* fd) {
            *fd = ::memfd_create("fake-syncobj", MFD_CLOEXEC);
            return *fd >= 0 ? 0 : -errno;
        };
        o.fd_to_handle = [](void*, int, unsigned* h) {
            *h = 1;
            return 0;
        };
        o.query = [](void* u, unsigned h, unsigned long long point, unsigned long long,
                     bool* done) {
            *done = of(u)->objs[h].signalled >= point;
            return 0;
        };
        o.eventfd = [](void* u, unsigned h, unsigned long long point, int efd) {
            auto* t = of(u);
            t->objs[h].eventfd = efd;
            t->objs[h].wait_point = point;
            ++t->eventfds;
            return 0;
        };
        o.signal = [](void* u, unsigned h, unsigned long long point) {
            of(u)->objs[h].signalled = std::max<std::uint64_t>(of(u)->objs[h].signalled, point);
            return 0;
        };
        o.import_sync_file = [](void* u, unsigned h, unsigned long long point, int sync_file) {
            auto& o2 = of(u)->objs[h];
            o2.last_import = point;
            ++o2.imports;
            pollfd pf{sync_file, POLLIN, 0};
            if (sync_file >= 0 && ::poll(&pf, 1, 1000) >= 0)
                ++o2.valid_fences;
            o2.signalled = std::max<std::uint64_t>(o2.signalled, 0); // the acquire is the GPU's
            return 0;
        };
        o.destroy = [](void* u, unsigned h) { of(u)->objs[h].used = false; };
        return o;
    }
    /// The compositor releases `h` at `point`: signals and wakes the eventfd.
    void release(unsigned h, std::uint64_t point) {
        objs[h].signalled = std::max(objs[h].signalled, point);
        if (objs[h].eventfd >= 0 && objs[h].wait_point <= point) {
            const std::uint64_t one = 1;
            (void)::write(objs[h].eventfd, &one, sizeof(one));
        }
    }
};

/// The ring without a compositor: ages under round-robin release, acquire with every image
/// busy, resize.
void ring_cases(lx::gfx::client_gpu& gpu, const lx::gfx::format_choice& format, const char* tier) {
    lx::gfx::client_swapchain chain;
    window listener{};
    listener.make_buffers = false;
    chain.set_listener(&listener);
    lx::gfx::swapchain_options o{};
    o.width = 64;
    o.height = 48;
    o.format = format;
    o.explicit_sync = false;
    if (auto made = chain.create(gpu, o); !made) {
        check(std::format("{}: ring swapchain", tier).c_str(), false,
              lx::format_error(made.get_error()));
        return;
    }
    const unsigned n = chain.image_count();
    std::vector<unsigned> ages;
    std::vector<std::uint32_t> ids;
    lx::gfx::damage_region full{};
    full.set_full();
    renderers r{};
    (void)r.init(gpu);
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({64, 48});
    list->batch_for_render();
    const auto present = [&](lx::gfx::swapchain_image* img) {
        ages.push_back(img->age);
        ids.push_back(img->id);
        (void)r.draw(*img, ui_golden::stream_of(*list), nullptr, full, lx::color::rgb(0, 0, 0));
        return chain.present(*img, full).ok();
    };
    bool ok = true;
    for (unsigned i = 0; i < n; ++i) {
        auto img = chain.acquire();
        ok = ok && img && img.value() && present(img.value());
    }
    const std::uint64_t t0 = now_ns();
    auto none = chain.acquire();
    const double none_us = static_cast<double>(now_ns() - t0) / 1e3;
    const bool got_none = none && none.value() == nullptr;
    chain.release(ids[0]);
    auto again = chain.acquire();
    const bool round_robin = again && again.value() && again.value()->id == ids[0];
    if (round_robin)
        ages.push_back(again.value()->age);
    lx::gfx::damage_region age_damage{};
    if (round_robin)
        chain.add_age_damage(*again.value(), age_damage);
    std::string seq;
    for (unsigned a : ages)
        seq += std::format("{} ", a);
    const bool age_ok = ages.size() == n + 1 && ages[n] == n &&
                        std::all_of(ages.begin(), ages.begin() + n, [](unsigned a) { return a == 0; });
    check(std::format("{}: buffer age {}×0 then {} under round-robin", tier, n, n).c_str(),
          ok && round_robin && age_ok, std::format("ages {}", seq));
    check(std::format("{}: acquire with every image busy returns none, without blocking", tier).c_str(),
          got_none && chain.stats().acquire_none == 1 && none_us < 1000.0,
          std::format("{:.1f} µs", none_us));
    check(std::format("{}: age damage of an age-{} image is the {} frames it missed (full here)",
                      tier, n, n - 1)
              .c_str(),
          age_damage.full, "every missed frame was a full repaint");
    if (round_robin) {
        lx::gfx::damage_region small{};
        small.add({1, 2, 3, 4});
        (void)present(again.value());
        // Release all; the next acquire takes the newest content (age 1) and misses nothing.
        for (std::uint32_t id : ids)
            chain.release(id);
        auto young = chain.acquire();
        lx::gfx::damage_region dmg{};
        if (young && young.value())
            chain.add_age_damage(*young.value(), dmg);
        check(std::format("{}: acquire prefers the newest content (age 1, no damage to repair)", tier)
                  .c_str(),
              young && young.value() && young.value()->age == 1 && dmg.empty(),
              young && young.value() ? std::format("age {}", young.value()->age) : "none");
        if (young && young.value())
            chain.cancel(*young.value());
    }

    // Resize: the busy images wait for their release, the free ones go now.
    for (std::uint32_t id : ids)
        chain.release(id);
    unsigned held = 0;
    std::vector<std::uint32_t> busy_ids;
    for (unsigned i = 0; i < n; ++i) {
        auto img = chain.acquire();
        if (img && img.value() && (i == 0 || i == 1) && present(img.value())) {
            ++held;
            busy_ids.push_back(img.value()->id);
        } else if (img && img.value()) {
            chain.cancel(*img.value());
        }
    }
    const unsigned destroyed0 = listener.destroyed;
    const unsigned alloc0 = chain.stats().allocations;
    auto resized = chain.resize(80, 60);
    const unsigned freed_now = listener.destroyed - destroyed0;
    auto same = chain.resize(80, 60);
    const bool retired_ok = chain.retired() == held;
    chain.release(busy_ids[0]);
    const unsigned after_one = listener.destroyed - destroyed0;
    chain.release(busy_ids[1]);
    const unsigned after_two = listener.destroyed - destroyed0;
    check(std::format("{}: resize reallocates once per new size, old images freed only after release",
                      tier)
              .c_str(),
          resized && resized.value() && same && !same.value() &&
              chain.stats().allocations == alloc0 + 1 && retired_ok && freed_now == n - held &&
              after_one == freed_now + 1 && after_two == freed_now + 2 && chain.retired() == 0,
          std::format("{} busy retired, {} free destroyed at resize, then {} / {} after releases; "
                      "allocations {}",
                      held, freed_now, after_one, after_two, chain.stats().allocations));
    // A storm of 30 sizes, one frame each, the compositor releasing every frame.
    const unsigned alloc1 = chain.stats().allocations;
    const unsigned made1 = chain.stats().images_created;
    bool storm = true;
    for (int i = 0; i < 30; ++i) {
        storm = storm && chain.resize(static_cast<unsigned>(60 + i), static_cast<unsigned>(40 + i)).ok();
        auto img = chain.acquire();
        storm = storm && img && img.value() && present(img.value());
        if (img && img.value())
            chain.release(img.value()->id);
    }
    check(std::format("{}: 30-size storm: 30 allocations, one image each, none left retired", tier)
              .c_str(),
          storm && chain.stats().allocations == alloc1 + 30 &&
              chain.stats().images_created == made1 + 30 && chain.retired() == 0,
          std::format("allocations +{}, images +{}", chain.stats().allocations - alloc1,
                      chain.stats().images_created - made1));
    r.shutdown();
    chain.destroy();
    check(std::format("{}: destroy reports every image destroyed", tier).c_str(),
          listener.created == listener.destroyed,
          std::format("{} created, {} destroyed", listener.created, listener.destroyed));
}

struct tier_result {
    bool presented = false;
};

/// Three golden frames through the compositor, read back from its side; then the steady
/// loop (allocations, present cost).
void compositor_cases(lx::wayland::display& d, lx::gfx::client_gpu& gpu,
                      const lx::gfx::format_choice& format, const char* tier, const char* label) {
    const std::string app_id = std::format("lumen.test.swapchain.{}", tier);
    window w{};
    lx::gfx::client_swapchain chain;
    w.chain = &chain;
    chain.set_listener(&w);
    chain.set_owner_affinity(lx::runtime::affinity::ui);
    if (!w.map(d, app_id.c_str())) {
        check(std::format("{}: window mapped", tier).c_str(), false, "no configure");
        return;
    }
    lx::gfx::swapchain_options o{};
    o.width = k_w;
    o.height = k_h;
    o.format = format;
    // The headless compositor has no syncobj manager: implicit sync on the wire.
    o.explicit_sync = d.has(lx::wayland::global_kind::syncobj);
    if (auto made = chain.create(gpu, o); !made) {
        check(std::format("{}: swapchain {}×{}", tier, k_w, k_h).c_str(), false,
              lx::format_error(made.get_error()));
        w.destroy();
        return;
    }
    renderers r{};
    if (auto ready = r.init(gpu); !ready) {
        check(std::format("{}: renderer", tier).c_str(), false, lx::format_error(ready.get_error()));
        chain.destroy();
        w.destroy();
        return;
    }
#if defined(LUMEN_HAS_EGL)
    gl_reader reader{};
    const bool have_reader = gpu.tier() != lx::gfx::client_tier::cpu && reader.init();
#endif

    auto atlas = std::make_unique<lx::gfx::atlas>();
    ui_golden::tiles tiles{};
    const bool tiles_ok = ui_golden::make_tiles(*atlas, tiles);
    lx::gfx::damage_region full{};
    full.set_full();
    for (unsigned f = 0; f < 3; ++f) {
        const ui_golden::golden_case* c = golden_case_named(k_frames[f]);
        if (!c || !tiles_ok) {
            check(std::format("{}: golden case {}", tier, k_frames[f]).c_str(), false, "missing");
            continue;
        }
        auto list = std::make_unique<lx::scene::draw_list>();
        ui_golden::build(*c, tiles, *list);
        auto img = chain.acquire();
        if (!img || !img.value()) {
            check(std::format("{}: frame {} acquired", tier, f).c_str(), false,
                  img ? "none free" : lx::format_error(img.get_error()));
            continue;
        }
        lx::gfx::swapchain_image& image = *img.value();
        lx::gfx::damage_region dmg{};
        chain.add_age_damage(image, dmg);
        const bool drawn = r.draw(image, ui_golden::stream_of(*list), atlas.get(), full,
                                  ui_golden::premultiplied(c->clear));
        auto presented = chain.present(image, full);
        w.commit(image, full);
        (void)d.roundtrip();
        tick();
        (void)d.roundtrip();

        std::vector<std::uint32_t> px;
        int stride = 0;
        std::string how;
        bool same_buffer = true;
        if (gpu.tier() == lx::gfx::client_tier::cpu) {
            int ow = 0, oh = 0;
            std::vector<std::uint32_t> out;
            const auto rec = record(app_id.c_str());
            if (capture_output(out, ow, oh) && rec.geometry.x >= 0 && rec.geometry.y >= 0 &&
                rec.geometry.x + c->width <= ow && rec.geometry.y + c->height <= oh) {
                px.resize(static_cast<std::size_t>(c->width) * c->height);
                for (int y = 0; y < c->height; ++y)
                    std::memcpy(px.data() + static_cast<std::size_t>(y) * c->width,
                                out.data() + static_cast<std::size_t>(rec.geometry.y + y) * ow +
                                    rec.geometry.x,
                                static_cast<std::size_t>(c->width) * 4u);
                stride = c->width;
                how = std::format("output capture at {},{}", rec.geometry.x, rec.geometry.y);
            }
        } else {
            received got = compositor_dmabuf(app_id.c_str());
            same_buffer = got.ok && same_file(got.desc.planes[0].fd.get(), image.planes[0].fd);
            if (got.ok && map_read(got.desc, px)) {
                stride = static_cast<int>(got.desc.width);
                how = "mapped";
            }
#if defined(LUMEN_HAS_EGL)
            else if (got.ok && have_reader && reader.read(got.desc, px)) {
                stride = static_cast<int>(got.desc.width);
                how = "EGL import";
            }
#endif
            if (got.ok)
                how += std::format(" mod {:#x} stride {} fourcc {:#x}", got.desc.planes[0].modifier,
                                   got.desc.planes[0].stride, got.desc.format);
        }
        ui_golden::diff diff{};
        if (stride)
            diff = ui_golden::compare_xrgb(*c, px.data(), stride, ui_golden::k_tolerance_vs_cpu);
        check(std::format("{}: frame {} ({}) reaches the compositor within G10 of the CPU golden{}",
                          tier, f + 1, c->name, label)
                  .c_str(),
              drawn && presented && same_buffer && stride && diff.ok() && w.failed == 0,
              std::format("{}; age {}; max error {} ({} px over), {}", how.empty() ? "no readback" : how,
                          image.age, diff.max_error, diff.over,
                          same_buffer ? "the committed dma-buf" : "a different buffer"));
    }

    // ── steady frames: a 48×48 quad moving over the window, damage-scoped ──
    const double load = load_average();
    auto scene = std::make_unique<lx::scene::draw_list>();
    std::vector<double> cost_us;
    std::vector<double> stage_us[4];
    std::uint64_t news = 0, mallocs = 0, frees = 0;
    unsigned waits = 0;
    lx::rect2i prev{0, 0, static_cast<int>(k_w), static_cast<int>(k_h)};
    constexpr unsigned k_frames_steady = 330;
    constexpr unsigned k_warm = 30;
    for (unsigned f = 0; f < k_frames_steady; ++f) {
        const int x = static_cast<int>((f * 7) % (k_w - 48));
        const int y = static_cast<int>((f * 3) % (k_h - 48));
        const lx::rect2i box{x, y, 48, 48};
        scene->begin({static_cast<int>(k_w), static_cast<int>(k_h)});
        {
            lx::ui::draw_scope s{*scene, {0, 0, static_cast<int>(k_w), static_cast<int>(k_h)}};
            s.quad({0, 0, static_cast<int>(k_w), static_cast<int>(k_h)},
                   ui_golden::solid(ui_golden::k_editor));
            s.quad(box, ui_golden::bordered(ui_golden::k_element_active, ui_golden::k_accent, 1.f, 6.f));
        }
        scene->batch_for_render();
        lx::gfx::damage_region frame_damage{};
        frame_damage.add(prev);
        frame_damage.add(box);
        prev = box;

        const bool counted = f >= k_warm;
        const std::uint64_t n0 = g_new_calls.load(), m0 = g_malloc_calls.load(),
                            fr0 = g_free_calls.load();
        g_counting = counted;
        const std::uint64_t t0 = now_ns();
        auto img = chain.acquire();
        if (img && !img.value()) {
            // Every image with the compositor: wait for a release (its frame), then retry.
            g_counting = false;
            ++waits;
            for (int i = 0; i < 20 && img && !img.value(); ++i) {
                tick();
                (void)d.dispatch(5);
                if (chain.explicit_sync())
                    (void)chain.collect_releases();
                img = chain.acquire();
            }
            g_counting = counted;
        }
        if (!img || !img.value()) {
            g_counting = false;
            check(std::format("{}: steady frame {} acquired", tier, f).c_str(), false, "stalled");
            break;
        }
        lx::gfx::swapchain_image& image = *img.value();
        lx::gfx::damage_region dmg = frame_damage;
        chain.add_age_damage(image, dmg);
        const std::uint64_t ta = now_ns();
        (void)r.draw(image, ui_golden::stream_of(*scene), nullptr, dmg, std::nullopt);
        const std::uint64_t td = now_ns();
        (void)chain.present(image, frame_damage);
        const std::uint64_t tp = now_ns();
        w.commit(image, frame_damage);
        const std::uint64_t t1 = now_ns();
        g_counting = false;
        if (counted) {
            stage_us[0].push_back(static_cast<double>(ta - t0) / 1e3);
            stage_us[1].push_back(static_cast<double>(td - ta) / 1e3);
            stage_us[2].push_back(static_cast<double>(tp - td) / 1e3);
            stage_us[3].push_back(static_cast<double>(t1 - tp) / 1e3);
            cost_us.push_back(static_cast<double>(t1 - t0) / 1e3);
            news += g_new_calls.load() - n0;
            mallocs += g_malloc_calls.load() - m0;
            frees += g_free_calls.load() - fr0;
        }
        (void)d.dispatch(0);
        tick();
        (void)d.dispatch(0);
    }
    const percentiles p = stats_of(cost_us);
    if (r.gl)
        measure(std::format("gl draw: ring-slot fence waits {:.1f} µs per frame on average",
                            cost_us.empty() ? 0.0
                                            : static_cast<double>(r.slot_wait_ns) / 1e3 /
                                                  static_cast<double>(k_frames_steady)));
    if (gpu.tier() == lx::gfx::client_tier::gl)
        measure(std::format("gl allocation: gbm path {} (2 gbm_bo_create_with_modifiers2, 1 "
                            "…_with_modifiers, 0 gbm_bo_create), modifier {:#x}, {} plane(s)",
                            chain.stats().gbm_path, format.modifiers[0], 1));
#if defined(LUMEN_TEST_SANITIZED)
    skip(std::format("{}: 0 allocations per steady present", tier).c_str(),
         "sanitizer build: allocation hooks are the sanitizer's");
#else
    if (gpu.tier() == lx::gfx::client_tier::vulkan && std::getenv("VK_INSTANCE_LAYERS")) {
        skip(std::format("{}: 0 operator new per steady present (G5)", tier).c_str(),
             std::format("a Vulkan layer is loaded (VK_INSTANCE_LAYERS): its own C++ allocations "
                         "({} new) are counted",
                         news));
    } else
    check(std::format("{}: 0 operator new per steady present (G5), over {} frames", tier,
                      cost_us.size())
              .c_str(),
          news == 0 && !cost_us.empty(),
          std::format("{} new; malloc {} / free {} from libwayland and the driver ({:.1f} / frame, "
                      "net {})",
                      news, mallocs, frees,
                      cost_us.empty() ? 0.0 : static_cast<double>(mallocs) / cost_us.size(),
                      static_cast<long long>(mallocs) - static_cast<long long>(frees)));
#endif
    measure(std::format("present cost {} {}×{} (acquire → draw 2 rects + age → present → "
                        "attach/damage/commit), {} frames: p50 {:.1f} µs p95 {:.1f} µs max {:.1f} "
                        "µs; {} waits for a release; {} images made; load {:.2f}{}",
                        tier, k_w, k_h, cost_us.size(), p.p50, p.p95, p.max, waits,
                        chain.stats().images_created, load, label));
    measure(std::format("present stages {} p50/p95 µs: acquire {:.1f}/{:.1f}, draw {:.1f}/{:.1f}, "
                        "present {:.1f}/{:.1f}, attach+damage+commit+flush {:.1f}/{:.1f}",
                        tier, stats_of(stage_us[0]).p50, stats_of(stage_us[0]).p95,
                        stats_of(stage_us[1]).p50, stats_of(stage_us[1]).p95,
                        stats_of(stage_us[2]).p50, stats_of(stage_us[2]).p95,
                        stats_of(stage_us[3]).p50, stats_of(stage_us[3]).p95));
    if (gpu.tier() != lx::gfx::client_tier::cpu) {
        // The bench hook: the same frame, then the wait for its GPU work (wait_presented).
        std::vector<double> gpu_us;
        for (unsigned f = 0; f < 60; ++f) {
            auto img = chain.acquire();
            for (int i = 0; i < 20 && img && !img.value(); ++i) {
                tick();
                (void)d.dispatch(5);
                img = chain.acquire();
            }
            if (!img || !img.value())
                break;
            lx::gfx::damage_region dmg{};
            dmg.add({0, 0, 64, 64});
            chain.add_age_damage(*img.value(), dmg);
            (void)r.draw(*img.value(), ui_golden::stream_of(*scene), nullptr, dmg, std::nullopt);
            (void)chain.present(*img.value(), dmg);
            auto waited = chain.wait_presented();
            w.commit(*img.value(), dmg);
            if (waited)
                gpu_us.push_back(static_cast<double>(waited.value()) / 1e3);
            (void)d.dispatch(0);
            tick();
        }
        const percentiles g = stats_of(gpu_us);
        measure(std::format("gpu wait after present {} (wait_presented: {}), {} frames: p50 {:.1f} "
                            "µs p95 {:.1f} µs{}",
                            tier, gpu.tier() == lx::gfx::client_tier::gl ? "glFinish" : "the frame's fence",
                            gpu_us.size(), g.p50, g.p95, label));
    }
    r.shutdown();
    chain.destroy();
    w.destroy();
    (void)d.roundtrip();
}

/// GL explicit sync against the fake kernel timeline; then the implicit fallback by option.
void gl_sync_cases(lx::gfx::client_gpu& gpu, const lx::gfx::format_choice& format) {
    if (gpu.tier() != lx::gfx::client_tier::gl) {
        skip("gl: explicit sync (native fence → acquire point)", "no GL tier on this host");
        return;
    }
    if (!gpu.egl()->supports_native_fence()) {
        skip("gl: explicit sync (native fence → acquire point)", "no EGL_ANDROID_native_fence_sync");
        return;
    }
    fake_timeline fake{};
    lx::gfx::client_swapchain chain;
    lx::gfx::swapchain_options o{};
    o.width = 64;
    o.height = 48;
    o.format = format;
    o.explicit_sync = true;
    o.timeline = fake.ops();
    renderers r{};
    bool ok = chain.create(gpu, o).ok() && r.init(gpu).ok();
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({64, 48});
    list->batch_for_render();
    lx::gfx::damage_region full{};
    full.set_full();
    std::vector<std::uint64_t> acquire_points;
    std::vector<unsigned> handles;
    for (unsigned i = 0; ok && i < 3; ++i) {
        auto img = chain.acquire();
        ok = img && img.value();
        if (!ok)
            break;
        acquire_points.push_back(img.value()->acquire_point);
        ok = img.value()->timeline_fd >= 0 &&
             r.draw(*img.value(), ui_golden::stream_of(*list), nullptr, full, lx::color::rgb(0, 0, 0)) &&
             chain.present(*img.value(), full).ok();
    }
    unsigned imports = 0, valid = 0;
    for (const auto& obj : fake.objs) {
        imports += obj.imports;
        valid += obj.valid_fences;
    }
    check("gl: explicit sync — each frame's native fence is imported at its acquire point",
          ok && chain.explicit_sync() && chain.stats().native_fences == 3 && imports == 3 &&
              valid == 3 && fake.creates == 3 && fake.eventfds == 3,
          std::format("{} native fences, {} imports ({} real sync_files), {} syncobjs, {} release "
                      "eventfds; acquire points {} {} {} (fake kernel timeline: vmwgfx has none)",
                      chain.stats().native_fences, imports, valid, fake.creates, fake.eventfds,
                      acquire_points.size() > 0 ? acquire_points[0] : 0,
                      acquire_points.size() > 1 ? acquire_points[1] : 0,
                      acquire_points.size() > 2 ? acquire_points[2] : 0));
    auto none = chain.acquire();
    const bool blocked = none && none.value() == nullptr;
    chain.release(1); // wl_buffer.release is undefined under explicit sync: ignored
    const unsigned freed_by_release = chain.collect_releases();
    // The compositor signals the first image's release point (2) and wakes the eventfd.
    fake.release(1, 2);
    pollfd pf{chain.release_fd(), POLLIN, 0};
    const bool woke = ::poll(&pf, 1, 0) == 1;
    const unsigned freed = chain.collect_releases();
    auto next = chain.acquire();
    check("gl: explicit sync — busy until the release point signals; the eventfd wakes the loop",
          blocked && freed_by_release == 0 && woke && freed == 1 && next && next.value() &&
              next.value()->age == 3 && next.value()->acquire_point == 3 &&
              next.value()->release_point == 4,
          std::format("none while busy {}, eventfd readable {}, freed {}, next age {} points {}/{}",
                      blocked, woke, freed, next && next.value() ? next.value()->age : 0,
                      next && next.value() ? next.value()->acquire_point : 0,
                      next && next.value() ? next.value()->release_point : 0));
    if (next && next.value())
        chain.cancel(*next.value());
    r.shutdown();
    chain.destroy();

    lx::gfx::client_swapchain implicit;
    o.disable_native_fence = true;
    ok = implicit.create(gpu, o).ok() && r.init(gpu).ok();
    auto img = ok ? implicit.acquire() : lx::result<lx::gfx::swapchain_image*>{nullptr};
    ok = ok && img && img.value() &&
         r.draw(*img.value(), ui_golden::stream_of(*list), nullptr, full, lx::color::rgb(0, 0, 0)) &&
         implicit.present(*img.value(), full).ok();
    check("gl: implicit fallback forced — no syncobj, no fence, wl_buffer.release frees the image",
          ok && !implicit.explicit_sync() && implicit.stats().native_fences == 0 &&
              img.value()->timeline_fd < 0 && implicit.release_fd() < 0,
          std::format("explicit {}, fences {}", implicit.explicit_sync(),
                      implicit.stats().native_fences));
    r.shutdown();
    implicit.destroy();
}

void run_cases() {
    pure_cases();

    lx::wayland::display d;
    if (auto c = d.connect({.name = k_socket}); !c) {
        check("connect", false, lx::format_error(c.get_error()));
        return;
    }
    lx::wayland::dmabuf_feedback* fb = d.default_feedback();
    std::vector<lx::gfx::client_format> storage;
    std::vector<lx::gfx::client_tranche> tranches;
    lx::gfx::client_feedback feedback{};
    if (fb && fb->received()) {
        feedback = feedback_of(fb->snapshot(), storage, tranches);
        std::string list;
        for (const auto& t : tranches) {
            list += std::format("[target {:#x} scanout {}:", t.target_device, t.scanout);
            for (const auto& f : t.formats)
                list += std::format(" {:#010x}/{:#x}", f.fourcc, f.modifier);
            list += "]";
        }
        measure(std::format("headless compositor dma-buf feedback: main device {:#x}, {} "
                            "tranche(s) {}; syncobj manager {}",
                            feedback.main_device, tranches.size(), list,
                            d.has(lx::wayland::global_kind::syncobj) ? "advertised" : "not advertised"));
    }

    // Automatic, as a window would choose against this compositor.
    {
        lx::gfx::client_gpu gpu;
        auto made = gpu.create(feedback, {.read_environment = false});
        check("automatic: the headless compositor names no device → cpu",
              made && gpu.tier() == lx::gfx::client_tier::cpu &&
                  std::strlen(gpu.refusal(lx::gfx::client_tier::gl)) > 0,
              std::format("tier {}; gl: {}", lx::gfx::client_tier_name(gpu.tier()),
                          gpu.refusal(lx::gfx::client_tier::gl)));
    }
    auto node = lx::gfx::open_render_node(0);
    const std::uint64_t render_dev = node ? node.value().dev : 0;
    if (node)
        measure(std::format("render node {} dev {:#x}", node.value().path, render_dev));
    if (render_dev) {
        // What a window on a compositor that names this node gets, against what this host
        // has: hardware Vulkan devices (the probe the client makes, software ICDs skipped)
        // and whether forced GL finds a hardware GL_RENDERER (forced without allow_software
        // refuses a software one).
        lx::gfx::physical_device_list probed{};
        {
            const lx::gfx::hardware_probe_scope hardware_only;
            probed = lx::gfx::device_selector::probe_devices();
        }
        unsigned vk_hardware = 0;
        for (unsigned i = 0; i < probed.count; ++i)
            vk_hardware += probed.devices[i].kind != lx::gfx::physical_device_kind::cpu ? 1u : 0u;
        bool gl_hardware = false;
        std::string gl_name;
        {
            lx::gfx::client_gpu glp;
            gl_hardware = glp.create(feedback, {.force = lx::gfx::client_tier::gl,
                                                .read_environment = false,
                                                .device_override = render_dev})
                              .ok() &&
                          !glp.software();
            gl_name = glp.name();
        }
        lx::gfx::client_gpu gpu;
        auto made = gpu.create(feedback, {.read_environment = false, .device_override = render_dev});
        const std::string detail =
            std::format("tier {} ({}){}; {} hardware Vulkan device(s); forced GL {} ({}); "
                        "vulkan: {}; gl: {}",
                        lx::gfx::client_tier_name(gpu.tier()), gpu.name(),
                        gpu.software() ? " software" : "", vk_hardware,
                        gl_hardware ? "hardware" : "refused or software", gl_name,
                        gpu.refusal(lx::gfx::client_tier::vulkan),
                        gpu.refusal(lx::gfx::client_tier::gl));
        if (vk_hardware > 0) {
            // Which device owns the node is the selector's to say: not determinable here.
            skip("automatic on the render node", "a hardware Vulkan device exists: " + detail);
        } else if (gl_hardware) {
            check("automatic on the render node: no hardware Vulkan → vulkan refused; hardware "
                  "GL_RENDERER → gl",
                  made && gpu.tier() == lx::gfx::client_tier::gl && !gpu.software() &&
                      std::strlen(gpu.refusal(lx::gfx::client_tier::vulkan)) > 0,
                  detail);
        } else {
            check("automatic on the render node: no hardware Vulkan or GL → cpu, both refused",
                  made && gpu.tier() == lx::gfx::client_tier::cpu &&
                      std::strlen(gpu.refusal(lx::gfx::client_tier::vulkan)) > 0 &&
                      std::strlen(gpu.refusal(lx::gfx::client_tier::gl)) > 0,
                  detail);
        }
    } else {
        skip("automatic on the render node", "no render node");
    }

    // The tiers, each forced (the override a test or bench uses).
    lx::gfx::format_choice shm_format{};
    {
        lx::gfx::client_gpu gpu;
        if (auto made = gpu.create(feedback, {.force = lx::gfx::client_tier::cpu}); made) {
            ring_cases(gpu, shm_format, "cpu");
            compositor_cases(d, gpu, shm_format, "cpu", "");
        } else {
            check("cpu: tier", false, lx::format_error(made.get_error()));
        }
    }
    {
        lx::gfx::client_gpu gpu;
        auto made = gpu.create(feedback, {.force = lx::gfx::client_tier::gl,
                                          .device_override = render_dev});
        auto format = made ? lx::gfx::choose_client_format(feedback, {.opaque = true}, 0)
                           : lx::result<lx::gfx::format_choice>{lx::gfx::format_choice{}};
        if (!made) {
            skip("gl tier", lx::format_error(made.get_error()));
        } else if (!format) {
            check("gl: a format from the feedback", false, lx::format_error(format.get_error()));
        } else {
            const std::string label = gpu.software() ? " [software GL]" : "";
            measure(std::format("gl tier: {} on {}", gpu.name(), gpu.node().path));
            ring_cases(gpu, format.value(), "gl");
            gl_sync_cases(gpu, format.value());
            compositor_cases(d, gpu, format.value(), "gl", label.c_str());
        }
    }
    {
        lx::gfx::client_gpu gpu;
        auto made = gpu.create(feedback, {.force = lx::gfx::client_tier::vulkan,
                                          .allow_software = true,
                                          .device_override = render_dev});
        auto format = made ? lx::gfx::choose_client_format(feedback, {.opaque = true}, 0)
                           : lx::result<lx::gfx::format_choice>{lx::gfx::format_choice{}};
        if (!made) {
            skip("vulkan tier (forced, software allowed)", lx::format_error(made.get_error()));
        } else if (!format) {
            check("vulkan: a format from the feedback", false, lx::format_error(format.get_error()));
        } else {
            lx::gfx::client_swapchain probe;
            lx::gfx::swapchain_options po{};
            po.width = 16;
            po.height = 16;
            po.format = format.value();
            po.explicit_sync = false;
            auto ok = probe.create(gpu, po);
            auto img = ok ? probe.acquire() : lx::result<lx::gfx::swapchain_image*>{ok.get_error()};
            if (!ok || !img || !img.value()) {
                skip("vulkan tier (forced on a software device)",
                     std::format("dma-buf export unsupported here: {}",
                                 !ok ? lx::format_error(ok.get_error())
                                     : (img ? "no image" : lx::format_error(img.get_error()))));
            } else {
                probe.cancel(*img.value());
                probe.destroy();
                const std::string label = gpu.software() ? " [software: lavapipe, correctness only]" : "";
                measure(std::format("vulkan tier: {}{}", gpu.name(), label));
                ring_cases(gpu, format.value(), "vulkan");
                compositor_cases(d, gpu, format.value(), "vulkan", label.c_str());
            }
        }
    }
    if (!d.has(lx::wayland::global_kind::syncobj))
        skip("explicit sync on the wire (set_acquire_point / set_release_point)",
             "the headless compositor advertises no wp_linux_drm_syncobj_manager_v1 and vmwgfx has "
             "no timeline syncobjs: needs a host with both");
    d.disconnect();
}

} // namespace

int main() {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.present = lx::compositor::present_backend::cpu;
    cfg.virtual_outputs = "1280x800";
    cfg.server_side_decorations = false;
    lx::compositor::compositor comp{cfg};
    // The compositor composites on the CPU and would import client dma-bufs through a
    // Vulkan device — lavapipe here, which cannot import vmwgfx buffers (it maps them). With
    // no Vulkan driver it keeps the bookkeeping import: every dma-buf commit is accepted and
    // registered as received (plane fds, offsets, strides, modifier), which is what the test
    // reads back. The client's own Vulkan tier loads its drivers after this.
    ::setenv("VK_LOADER_DRIVERS_DISABLE", "*", 1);
    auto started = comp.start();
    ::unsetenv("VK_LOADER_DRIVERS_DISABLE");
    if (!started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    std::atomic<bool> pump_on{true};
    std::mutex dispatch_lock;
    g_comp = &comp;
    g_lock = &dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int loop_fd = comp.wayland().event_fd();
        while (pump_on.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 2);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tick();

    run_cases();

    pump_on.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed, %d skipped\n", g_ran, g_failed, g_skipped);
    return g_failed == 0 ? 0 : 1;
}

#endif
