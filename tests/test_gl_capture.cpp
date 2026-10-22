// Screen capture on the GL backend (design-app-compat-capture D.4, CC1), against a real render
// node: a client's LINEAR XRGB dma-buf as a framebuffer, the blit of damage rects (and of a
// crop) from a composited scanout target, the pixel-pack read-back behind a native fence and
// its copy-out, the curtain clear, the GBM-backed offscreen buffer read off the render thread;
// then the capture engine end to end on GL — an output frame
// into dma-buf and into shm (fence, then the copy-out job), the pointer painted on a dma-buf
// copy, a window view composited into an offscreen buffer and read back, and the same shm
// copies read on a worker thread in one step.
//
// Skips (passes) without a hardware GL renderer with ES 3.0 and EGL dma-buf import.
#include "lumen_test.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(LUMEN_HAS_EGL)
#include <gbm.h>
#endif

import lx.foundation;
import lx.gfx;
import lx.compositor;

#if !defined(LUMEN_HAS_EGL)

int main() {
    std::printf("SKIP: built without EGL\n");
    return 0;
}

#else

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);

struct rig {
    int fd = -1;
    lx::gfx::egl_device device{};
    lx::gfx::gl_compositor gl{};
    lx::gfx::gl_capture cap{};
    gbm_device* gbm = nullptr;
    bool usable = false;
};

rig* shared() {
    static rig r{};
    static bool tried = false;
    if (!tried) {
        tried = true;
        r.fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (r.fd < 0)
            return nullptr;
        auto created = lx::gfx::egl_device::create(r.fd);
        if (!created) {
            std::printf("  (skip: %s)\n", created.get_error().message);
            return nullptr;
        }
        r.device = std::move(created).value();
        if (r.device.is_software_renderer() || !r.device.supports_dmabuf_import()) {
            std::printf("  (skip: software GL or no dma-buf import)\n");
            return nullptr;
        }
        if (!r.gl.initialize(r.device)) {
            std::printf("  (skip: GL compositor did not initialize)\n");
            return nullptr;
        }
        if (auto ok = r.cap.initialize(r.device); !ok) {
            std::printf("  (skip: %s)\n", ok.get_error().message);
            return nullptr;
        }
        r.gbm = gbm_create_device(r.fd);
        r.usable = r.gbm != nullptr;
    }
    return r.usable ? &r : nullptr;
}

/// A client's LINEAR XRGB dma-buf (what xdg-desktop-portal-wlr allocates for a screencast).
struct client_dmabuf {
    gbm_bo* bo = nullptr;
    int fd = -1;
    unsigned stride = 0;
    unsigned w = 0;
    unsigned h = 0;
    explicit client_dmabuf(gbm_device* gbm, unsigned width, unsigned height) : w(width), h(height) {
        const std::uint64_t linear = 0;
        bo = gbm_bo_create_with_modifiers2(gbm, width, height, GBM_FORMAT_XRGB8888, &linear, 1,
                                           GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bo)
            bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_XRGB8888,
                               GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (bo) {
            fd = gbm_bo_get_fd(bo);
            stride = gbm_bo_get_stride(bo);
        }
    }
    ~client_dmabuf() {
        if (fd >= 0)
            ::close(fd);
        if (bo)
            gbm_bo_destroy(bo);
    }
    /// The pixel at (x, y) as 0xRRGGBB (mapped for reading).
    std::uint32_t at(unsigned x, unsigned y) const {
        std::uint32_t s = 0;
        void* data = nullptr;
        void* p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &s, &data);
        if (!p)
            return 0xDEADBEEFu;
        const std::uint32_t v =
            *reinterpret_cast<const std::uint32_t*>(static_cast<const unsigned char*>(p) + y * s + x * 4u);
        gbm_bo_unmap(bo, data);
        return v & 0x00FFFFFFu;
    }
};

/// A 64x48 scanout target: black, a 16x16 blue square at (8, 8), a green one at (40, 24).
lx::gfx::gl_scanout_target make_source(rig& r) {
    auto target = lx::gfx::gl_scanout_target::create(r.device, 64, 48, k_xrgb);
    std::vector<unsigned char> blue(16u * 16u * 4u);
    std::vector<unsigned char> green(16u * 16u * 4u);
    for (std::size_t i = 0; i < blue.size(); i += 4) {
        blue[i + 2] = 255;
        blue[i + 3] = 255;
        green[i + 1] = 255;
        green[i + 3] = 255;
    }
    (void)r.gl.upload_rgba(101, 16, 16, blue.data());
    (void)r.gl.upload_rgba(102, 16, 16, green.data());
    lx::gfx::blit_command cmds[2]{};
    cmds[0].texture_id = 101;
    cmds[0].dst = {8, 8, 16, 16};
    cmds[0].blend = lx::blend_mode::opaque;
    cmds[1].texture_id = 102;
    cmds[1].dst = {40, 24, 16, 16};
    cmds[1].blend = lx::blend_mode::opaque;
    auto c = r.gl.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), cmds, 2, {});
    if (c && c.value().out_fence_fd >= 0)
        ::close(c.value().out_fence_fd);
    return std::move(target).value();
}

bool wait_fence(int fd) {
    if (fd < 0)
        return true;
    pollfd p{fd, POLLIN, 0};
    const bool ok = ::poll(&p, 1, 2000) == 1;
    ::close(fd);
    return ok;
}

lx::gfx::damage_region rect(lx::rect2i r) {
    lx::gfx::damage_region d{};
    d.add(r);
    return d;
}

} // namespace

LUMEN_TEST(gl_capture_blits_damage_rects_into_a_client_dmabuf) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    client_dmabuf client{r->gbm, 64, 48};
    LUMEN_CHECK(client.bo && client.fd >= 0);
    auto fb = r->cap.dmabuf_framebuffer(1, client.fd, 0, client.stride, 64, 48, k_xrgb, 0);
    LUMEN_CHECK(static_cast<bool>(fb));
    // Only the blue square's rect: the green one stays what the client had (cleared to red).
    LUMEN_CHECK(static_cast<bool>(r->cap.clear(fb.value(), 64, 48, lx::color::rgb(1.f, 0.f, 0.f))));
    LUMEN_CHECK(static_cast<bool>(r->cap.blit(src.framebuffer(), fb.value(), 64, 48, rect({8, 8, 16, 16}))));
    LUMEN_CHECK(wait_fence(r->cap.fence()));
    LUMEN_CHECK(client.at(10, 10) == 0x0000FFu);
    LUMEN_CHECK(client.at(45, 30) == 0xFF0000u); // outside the rect: untouched
    // The same buffer again is the cached framebuffer.
    auto again = r->cap.dmabuf_framebuffer(1, -1, 0, client.stride, 64, 48, k_xrgb, 0);
    LUMEN_CHECK(again && again.value() == fb.value());
    r->cap.forget_dmabuf(1);
    auto gone = r->cap.dmabuf_framebuffer(1, -1, 0, client.stride, 64, 48, k_xrgb, 0);
    LUMEN_CHECK(!gone);
}

LUMEN_TEST(gl_capture_blits_a_crop_to_the_buffer_origin) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    client_dmabuf client{r->gbm, 20, 20};
    auto fb = r->cap.dmabuf_framebuffer(2, client.fd, 0, client.stride, 20, 20, k_xrgb, 0);
    LUMEN_CHECK(static_cast<bool>(fb));
    LUMEN_CHECK(static_cast<bool>(r->cap.blit(src.framebuffer(), fb.value(), 64, 48,
                                              rect({36, 20, 20, 20}), 36, 20)));
    LUMEN_CHECK(wait_fence(r->cap.fence()));
    LUMEN_CHECK(client.at(0, 0) == 0 && client.at(5, 5) == 0x00FF00u && client.at(19, 19) == 0x00FF00u);
    r->cap.forget_dmabuf(2);
}

LUMEN_TEST(gl_capture_reads_back_through_a_pixel_pack_buffer_without_waiting) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    LUMEN_CHECK(static_cast<bool>(r->cap.read_async(7, src.framebuffer(), 64, 48, rect({0, 0, 64, 48}))));
    const int fence = r->cap.fence();
    LUMEN_CHECK(wait_fence(fence));
    std::vector<unsigned char> dst(64u * 48u * 4u, 0x5A);
    LUMEN_CHECK(static_cast<bool>(r->cap.finish_read(7, dst.data(), 256, 64, 48, rect({0, 0, 64, 48}))));
    const auto px = [&](unsigned x, unsigned y) {
        const unsigned char* p = &dst[(y * 64u + x) * 4u];
        return static_cast<std::uint32_t>(p[0]) | (p[1] << 8) | (p[2] << 16);
    };
    LUMEN_CHECK(px(10, 10) == 0x0000FFu && px(45, 30) == 0x00FF00u && px(0, 0) == 0);
    // Only the rects asked for, crop moved to the origin of a smaller buffer.
    std::vector<unsigned char> small(10u * 10u * 4u, 0x5A);
    LUMEN_CHECK(static_cast<bool>(r->cap.read_async(7, src.framebuffer(), 64, 48, rect({4, 4, 10, 10}))));
    LUMEN_CHECK(wait_fence(r->cap.fence()));
    LUMEN_CHECK(static_cast<bool>(r->cap.finish_read(7, small.data(), 40, 64, 48, rect({4, 4, 10, 10}),
                                                     4, 4, 10, 10)));
    LUMEN_CHECK(small[0] == 0 && small[(9 * 10 + 9) * 4] == 255); // (4,4) black, (13,13) blue
    r->cap.forget_owner(7);
}

// ── the engine on GL ─────────────────────────────────────────────────────────

namespace {

unsigned g_unref = 0;

struct engine_rig {
    lx::compositor::capture_shm_guards guards{};
    lx::compositor::capture_channel channel{};
    lx::compositor::capture_renderer renderer{};
    explicit engine_rig(rig& r) {
        lx::compositor::capture_shm_guards::hooks h{};
        h.begin_access = [](void*) {};
        h.end_access = [](void*) {};
        h.unref_pool = [](void*) { ++g_unref; };
        guards.bind(h);
        renderer.bind_gl(&r.gl, &r.device);
        renderer.bind_guards(&guards);
        channel.sessions.store(1);
    }
    std::vector<lx::compositor::capture_completion> drain() {
        renderer.flush(channel);
        lx::compositor::capture_completion c[16];
        const unsigned n = channel.drain(c, 16);
        return {c, c + n};
    }
};

lx::compositor::capture_frame_source source_of(const lx::gfx::gl_scanout_target& t) {
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(t.width()), static_cast<int>(t.height())};
    src.gl_framebuffer = t.framebuffer();
    return src;
}

} // namespace

LUMEN_TEST(gl_engine_copies_an_output_frame_into_a_dmabuf_behind_a_fence) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    engine_rig e{*r};
    client_dmabuf client{r->gbm, 64, 48};
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.target.dmabuf = true;
    j.target.width = 64;
    j.target.height = 48;
    j.target.buffer_key = 11;
    j.target.fd = ::fcntl(client.fd, F_DUPFD_CLOEXEC, 0);
    j.target.stride = client.stride;
    j.target.format = k_xrgb;
    (void)e.channel.post(j);
    e.renderer.take(e.channel);
    e.renderer.output_front(0, source_of(src), e.channel);
    auto done = e.drain();
    LUMEN_CHECK(done.size() == 1);
    LUMEN_CHECK(done[0].result == lx::compositor::capture_result::fence ||
                done[0].result == lx::compositor::capture_result::ready);
    LUMEN_CHECK(wait_fence(done[0].fence_fd));
    LUMEN_CHECK(client.at(10, 10) == 0x0000FFu && client.at(45, 30) == 0x00FF00u);
}

LUMEN_TEST(gl_engine_paints_the_pointer_on_a_dmabuf_copy) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    engine_rig e{*r};
    client_dmabuf client{r->gbm, 64, 48};
    std::vector<std::uint32_t> arrow(4u * 4u, 0xFFFFFFFFu);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.paint_cursors = true;
    j.target.dmabuf = true;
    j.target.width = 64;
    j.target.height = 48;
    j.target.buffer_key = 12;
    j.target.fd = ::fcntl(client.fd, F_DUPFD_CLOEXEC, 0);
    j.target.stride = client.stride;
    j.target.format = k_xrgb;
    (void)e.channel.post(j);
    e.renderer.take(e.channel);
    auto s = source_of(src);
    s.cursor.pixels = arrow.data();
    s.cursor.width = 4;
    s.cursor.height = 4;
    s.cursor.stride_px = 4;
    s.cursor.visible = true;
    s.cursor.generation = 1;
    s.cursor.rect = {30, 2, 4, 4};
    e.renderer.output_front(0, s, e.channel);
    auto done = e.drain();
    LUMEN_CHECK(done.size() == 1 && wait_fence(done[0].fence_fd));
    LUMEN_CHECK(client.at(31, 3) == 0xFFFFFFu && client.at(10, 10) == 0x0000FFu &&
                client.at(36, 3) == 0);
}

LUMEN_TEST(gl_engine_reads_an_output_frame_into_shm_in_two_steps) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    engine_rig e{*r};
    std::vector<unsigned char> mem(64u * 48u * 4u, 0x5A);
    const unsigned guard = e.guards.add(mem.data(), mem.data(), mem.data(), 256, 64, 48, true);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.target.width = 64;
    j.target.height = 48;
    j.target.shm = guard;
    (void)e.channel.post(j);
    e.renderer.take(e.channel);
    e.renderer.output_front(0, source_of(src), e.channel);
    auto first = e.drain();
    LUMEN_CHECK(first.size() == 1 && first[0].result == lx::compositor::capture_result::readback);
    LUMEN_CHECK(mem[(10 * 64 + 10) * 4] == 0x5A); // not copied out yet
    LUMEN_CHECK(wait_fence(first[0].fence_fd));
    lx::compositor::capture_job fin = j;
    fin.kind = lx::compositor::capture_job_kind::finish;
    (void)e.channel.post(fin);
    e.renderer.take(e.channel);
    auto second = e.drain();
    LUMEN_CHECK(second.size() == 1 && second[0].result == lx::compositor::capture_result::ready &&
                second[0].finished_read);
    const unsigned char* p = &mem[(10 * 64 + 10) * 4];
    LUMEN_CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 0xFF);
    e.guards.release(guard);
}

LUMEN_TEST(gl_engine_composites_a_window_view_into_an_offscreen_buffer_for_shm) {
    rig* r = shared();
    if (!r)
        return;
    (void)make_source(*r); // uploads the textures
    engine_rig e{*r};
    std::vector<unsigned char> mem(32u * 32u * 4u, 0x5A);
    const unsigned guard = e.guards.add(mem.data(), mem.data(), mem.data(), 128, 32, 32, false);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::toplevel;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.view = 16;
    j.view_frame = 3;
    j.target.width = 32;
    j.target.height = 32;
    j.target.shm = guard;
    (void)e.channel.post(j);
    e.renderer.take(e.channel);
    lx::gfx::blit_command cmd{};
    cmd.texture_id = 101;
    cmd.dst = {0, 0, 16, 16};
    cmd.blend = lx::blend_mode::opaque;
    lx::compositor::capture_frame_source s{};
    s.frame = 3;
    s.blits = &cmd;
    s.blit_count = 1;
    lx::gfx::damage_region all{};
    all.set_full();
    e.renderer.toplevel_composited(16, all, s, e.channel);
    auto first = e.drain();
    LUMEN_CHECK(first.size() == 1 && first[0].result == lx::compositor::capture_result::readback);
    LUMEN_CHECK(wait_fence(first[0].fence_fd));
    lx::compositor::capture_job fin = j;
    fin.kind = lx::compositor::capture_job_kind::finish;
    (void)e.channel.post(fin);
    e.renderer.take(e.channel);
    auto second = e.drain();
    LUMEN_CHECK(second.size() == 1 && second[0].result == lx::compositor::capture_result::ready);
    const unsigned char* in = &mem[(4 * 32 + 4) * 4];
    const unsigned char* out = &mem[(20 * 32 + 20) * 4];
    // The window's pixels; around them, transparent (a window has no backdrop).
    LUMEN_CHECK(in[0] == 255 && in[3] == 255 && out[0] == 0 && out[3] == 0);
    e.guards.release(guard);
}

namespace {

std::vector<std::thread> g_workers;
unsigned g_notified = 0;

bool spawn_worker(void (*fn)(void*), void* arg) {
    g_workers.emplace_back([fn, arg] { fn(arg); });
    return true;
}

void join_workers() {
    for (auto& t : g_workers)
        t.join();
    g_workers.clear();
}

} // namespace

LUMEN_TEST(gl_capture_offscreen_buffer_is_read_without_gl) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    auto off = r->cap.offscreen_framebuffer(9, 64, 48);
    LUMEN_CHECK(static_cast<bool>(off));
    void* bo = r->cap.offscreen_bo(9);
    if (!bo) {
        std::printf("  (skip: no GBM offscreen buffer)\n");
        r->cap.forget_owner(9);
        return;
    }
    LUMEN_CHECK(static_cast<bool>(r->cap.blit(src.framebuffer(), off.value(), 64, 48, rect({0, 0, 64, 48}))));
    LUMEN_CHECK(wait_fence(r->cap.fence()));
    std::vector<unsigned char> dst(10u * 10u * 4u, 0x5A);
    bool ok = false;
    std::thread t([&] {
        ok = lx::gfx::gl_capture::read_bo(bo, 64, 48, rect({4, 4, 10, 10}), dst.data(), 40, 4, 4,
                                          10, 10, true);
    });
    t.join();
    LUMEN_CHECK(ok);
    // (4,4) black, (13,13) blue; alpha forced opaque.
    LUMEN_CHECK(dst[0] == 0 && dst[3] == 0xFF && dst[(9 * 10 + 9) * 4] == 255);
    r->cap.forget_owner(9);
}

LUMEN_TEST(gl_engine_reads_shm_on_a_worker_in_one_step) {
    rig* r = shared();
    if (!r)
        return;
    auto src = make_source(*r);
    engine_rig e{*r};
    e.renderer.bind_worker(&spawn_worker, [](void*) { ++g_notified; }, nullptr);
    std::vector<unsigned char> mem(64u * 48u * 4u, 0x5A);
    const unsigned guard = e.guards.add(mem.data(), mem.data(), mem.data(), 256, 64, 48, true);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.target.width = 64;
    j.target.height = 48;
    j.target.shm = guard;
    (void)e.channel.post(j);
    e.renderer.take(e.channel);
    const unsigned before = g_notified;
    e.renderer.output_front(0, source_of(src), e.channel);
    join_workers();
    auto done = e.drain();
    LUMEN_CHECK(done.size() == 1 && done[0].result == lx::compositor::capture_result::ready);
    LUMEN_CHECK(g_notified == before + 1);
    const unsigned char* p = &mem[(10 * 64 + 10) * 4];
    LUMEN_CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 0xFF);
    e.guards.release(guard);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }

#endif
