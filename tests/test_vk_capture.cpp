// Screen capture on the Vulkan backend (design-app-compat-capture D.4): an output frame read
// back from the composited target into a client's shm buffer (wl_shm byte order, alpha
// opaque), a crop, and the pointer blended for paint_cursors; the asynchronous path — the
// copy submitted on render without a wait, the rows written by a worker, the slot's buffer
// reused frame after frame. A client's dma-buf as the copy's destination (imported once,
// rows placed at its stride on the GPU, damage-limited, the sync file of the copy), a window
// capture composited into the session's persistent target (shm read-back and dma-buf copy),
// and the pooling of it all (same-size read-backs and re-used buffers allocate nothing).
// Runs on whatever Vulkan device exists (lavapipe on a host without hardware Vulkan); skips
// without one — and the dma-buf tests without a device that imports dma-buf memory.
#include "lumen_test.hpp"
#include "render_node.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifdef LUMEN_HAS_EGL
#include <gbm.h>
#endif

import std;
import lx.foundation;
import lx.gfx;
import lx.compositor;

namespace {

constexpr unsigned k_w = 64;
constexpr unsigned k_h = 48;
constexpr lx::fourcc k_xr24 = 0x34325258u;

struct fixture {
    lx::gfx::device device{};
    lx::gfx::vulkan_compositor vk{};
    lx::gfx::render_target target{};
    /// The render node behind the connected card: where a client (GBM) allocates the
    /// dma-bufs the copy tests import — the same device the compositor's Vulkan runs on.
    int node_fd = -1;
    void* gbm = nullptr;
    bool dmabufs = false;
    bool usable = false;
};

fixture* gpu() {
    static fixture f{};
    static bool tried = false;
    if (tried)
        return f.usable ? &f : nullptr;
    tried = true;
    // The device behind the display's card, as the compositor selects it — so a client's
    // dma-buf of that node is native memory to the device the copy runs on.
    lx::gfx::drm_node card{};
    f.node_fd = lumen_test::open_render_node();
    if (f.node_fd >= 0) {
        struct stat st{};
        if (::fstat(f.node_fd, &st) == 0) {
            card.present = true;
            card.major = major(st.st_rdev);
            card.minor = minor(st.st_rdev);
        }
    }
    auto selected = lx::gfx::device_selector::select_best(card);
    if (!selected) {
        std::printf("  (skip: no Vulkan device)\n");
        return nullptr;
    }
    f.device = std::move(selected).value();
    if (auto ok = f.vk.initialize(f.device.context()); !ok) {
        std::printf("  (skip: %s)\n", ok.get_error().message);
        return nullptr;
    }
    auto t = lx::gfx::render_target::create(f.device.context(), k_w, k_h);
    if (!t) {
        std::printf("  (skip: %s)\n", t.get_error().message);
        return nullptr;
    }
    f.target = std::move(t).value();
#ifdef LUMEN_HAS_EGL
    if (f.vk.capture_dmabuf_supported() && f.node_fd >= 0 &&
        f.device.info().render_node.present &&
        f.device.info().render_node.major == card.major &&
        f.device.info().render_node.minor == card.minor) {
        f.gbm = gbm_create_device(f.node_fd);
        f.dmabufs = f.gbm != nullptr;
    }
#endif
    // Left half red, right half blue.
    std::vector<unsigned char> rgba(k_w * k_h * 4u);
    for (unsigned y = 0; y < k_h; ++y)
        for (unsigned x = 0; x < k_w; ++x) {
            unsigned char* p = &rgba[(y * k_w + x) * 4u];
            const bool left = x < k_w / 2;
            p[0] = left ? 255 : 0;
            p[2] = left ? 0 : 255;
            p[3] = 255;
        }
    (void)f.vk.upload_rgba(0x4000'0101u, k_w, k_h, rgba.data());
    lx::gfx::blit_command cmd{};
    cmd.texture_id = 0x4000'0101u;
    cmd.dst = {0, 0, static_cast<int>(k_w), static_cast<int>(k_h)};
    auto c = f.vk.composite(f.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    if (c && c.value().out_fence_fd >= 0)
        ::close(c.value().out_fence_fd);
    f.usable = static_cast<bool>(c);
    return f.usable ? &f : nullptr;
}

#ifdef LUMEN_HAS_EGL

/// A capture client's LINEAR XRGB dma-buf at GBM's own stride (what a screencast cycles
/// through). A width GBM does not align to 4 pixels makes the stride padded, which is the
/// placement the copy must honour.
struct client_dmabuf {
    gbm_bo* bo = nullptr;
    int fd = -1;
    unsigned stride = 0;
    unsigned w = 0;
    unsigned h = 0;
    explicit client_dmabuf(void* gbm_dev, unsigned width, unsigned height)
        : w(width), h(height) {
        auto* dev = static_cast<gbm_device*>(gbm_dev);
        const std::uint64_t linear = 0;
        bo = gbm_bo_create_with_modifiers2(dev, width, height, GBM_FORMAT_XRGB8888, &linear, 1,
                                           GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bo)
            bo = gbm_bo_create(dev, width, height, GBM_FORMAT_XRGB8888,
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
    client_dmabuf(const client_dmabuf&) = delete;
    client_dmabuf& operator=(const client_dmabuf&) = delete;
    /// (x, y) as B, G, R — reading the mapped buffer at GBM's stride.
    std::uint32_t at(unsigned x, unsigned y) const {
        std::uint32_t s = 0;
        void* data = nullptr;
        void* p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &s, &data);
        if (!p)
            return 0xDEADBEEFu;
        const std::uint32_t v = *reinterpret_cast<const std::uint32_t*>(
            static_cast<const unsigned char*>(p) + y * s + x * 4u);
        gbm_bo_unmap(bo, data);
        return v & 0x00FFFFFFu;
    }
    /// Every mapped byte to a pattern (so a copy that misses a row shows).
    void fill(unsigned char pattern) {
        std::uint32_t s = 0;
        void* data = nullptr;
        void* p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &s, &data);
        if (!p)
            return;
        std::memset(p, pattern, static_cast<std::size_t>(s) * h);
        gbm_bo_unmap(bo, data);
    }
};

/// A composited target of `w` x `h`, left half red, right half blue (as `gpu`'s).
lx::gfx::render_target red_blue_target(fixture& f, unsigned w, unsigned h) {
    auto t = lx::gfx::render_target::create(f.device.context(), w, h);
    if (!t)
        return {};
    std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4u);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            unsigned char* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4u];
            const bool left = x < w / 2;
            p[0] = left ? 255 : 0;
            p[2] = left ? 0 : 255;
            p[3] = 255;
        }
    (void)f.vk.upload_rgba(0x4000'0202u, w, h, rgba.data());
    lx::gfx::blit_command cmd{};
    cmd.texture_id = 0x4000'0202u;
    cmd.dst = {0, 0, static_cast<int>(w), static_cast<int>(h)};
    auto c = f.vk.composite(t.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    if (c && c.value().out_fence_fd >= 0)
        ::close(c.value().out_fence_fd);
    return static_cast<bool>(c) ? std::move(t).value() : lx::gfx::render_target{};
}

/// Waits a capture copy's sync file out (a -1 fence already completed).
void wait_fence(int fd) {
    if (fd < 0)
        return;
    pollfd p{fd, POLLIN, 0};
    (void)::poll(&p, 1, 2000);
    ::close(fd);
}

#endif // LUMEN_HAS_EGL

struct rig {
    lx::compositor::capture_shm_guards guards{};
    lx::compositor::capture_channel channel{};
    lx::compositor::capture_renderer renderer{};
    explicit rig(fixture& f) {
        lx::compositor::capture_shm_guards::hooks h{};
        h.begin_access = [](void*) {};
        h.end_access = [](void*) {};
        h.unref_pool = [](void*) {};
        guards.bind(h);
        renderer.bind_vulkan(&f.vk);
        renderer.bind_guards(&guards);
        channel.sessions.store(1);
    }
};

} // namespace

LUMEN_TEST(vk_output_frame_is_read_back_into_shm_in_wl_shm_order) {
    fixture* f = gpu();
    if (!f)
        return;
    rig r{*f};
    std::vector<unsigned char> mem(k_w * k_h * 4u, 0x5A);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.target.width = k_w;
    j.target.height = k_h;
    j.target.shm = r.guards.add(mem.data(), mem.data(), mem.data(), k_w * 4u, k_w, k_h, true);
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
    src.vk_target = &f->target;
    r.renderer.output_front(0, src, r.channel);
    lx::compositor::capture_completion c[4];
    const unsigned n = r.channel.drain(c, 4);
    LUMEN_CHECK(n == 1 && c[0].result == lx::compositor::capture_result::ready);
    const unsigned char* left = &mem[(20 * k_w + 10) * 4u];
    const unsigned char* right = &mem[(20 * k_w + 50) * 4u];
    // B, G, R, A in memory.
    LUMEN_CHECK(left[2] > 200 && left[0] < 60 && left[3] == 0xFF);
    LUMEN_CHECK(right[0] > 200 && right[2] < 60);
}

LUMEN_TEST(vk_crop_lands_at_the_buffer_origin) {
    fixture* f = gpu();
    if (!f)
        return;
    rig r{*f};
    std::vector<unsigned char> mem(8u * 8u * 4u, 0x5A);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.crop = {28, 10, 8, 8};
    j.target.width = 8;
    j.target.height = 8;
    j.target.shm = r.guards.add(mem.data(), mem.data(), mem.data(), 32, 8, 8, true);
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
    src.vk_target = &f->target;
    r.renderer.output_front(0, src, r.channel);
    lx::compositor::capture_completion c[4];
    LUMEN_CHECK(r.channel.drain(c, 4) == 1 && c[0].result == lx::compositor::capture_result::ready);
    // Columns 28..31 are red, 32..35 blue.
    LUMEN_CHECK(mem[(2 * 8 + 1) * 4 + 2] > 200 && mem[(2 * 8 + 6) * 4 + 0] > 200);
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

LUMEN_TEST(vk_read_lands_on_a_worker_and_the_slot_is_reused) {
    fixture* f = gpu();
    if (!f)
        return;
    rig r{*f};
    r.renderer.bind_worker(&spawn_worker, [](void*) { ++g_notified; }, nullptr);
    for (unsigned frame = 1; frame <= 3; ++frame) {
        std::vector<unsigned char> mem(k_w * k_h * 4u, 0x5A);
        lx::compositor::capture_job j{};
        j.kind = lx::compositor::capture_job_kind::output;
        j.session = 2;
        j.generation = 1;
        j.serial = frame;
        j.full = true;
        j.target.width = k_w;
        j.target.height = k_h;
        j.target.shm = r.guards.add(mem.data(), mem.data(), mem.data(), k_w * 4u, k_w, k_h, true);
        (void)r.channel.post(j);
        r.renderer.take(r.channel);
        lx::compositor::capture_frame_source src{};
        src.frame = frame;
        src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
        src.vk_target = &f->target;
        const unsigned notified = g_notified;
        r.renderer.output_front(0, src, r.channel);
        // Render returned with the copy in flight; the worker delivers the completion.
        join_workers();
        LUMEN_CHECK(g_notified == notified + 1);
        lx::compositor::capture_completion c[4];
        const unsigned n = r.channel.drain(c, 4);
        LUMEN_CHECK(n == 1 && c[0].result == lx::compositor::capture_result::ready &&
                    c[0].serial == frame);
        const unsigned char* left = &mem[(20 * k_w + 10) * 4u];
        const unsigned char* right = &mem[(20 * k_w + 50) * 4u];
        LUMEN_CHECK(left[2] > 200 && left[0] < 60 && left[3] == 0xFF);
        LUMEN_CHECK(right[0] > 200 && right[2] < 60 && right[3] == 0xFF);
        r.guards.release(j.target.shm);
    }
    // The session's end frees its slot (and a new one of the index starts afresh).
    lx::compositor::capture_job forget{};
    forget.kind = lx::compositor::capture_job_kind::forget_session;
    forget.session = 2;
    (void)r.channel.post(forget);
    r.renderer.take(r.channel);
    LUMEN_CHECK(f->vk.read_back_wait(1, 0) == nullptr);
}

LUMEN_TEST(vk_read_back_async_copies_only_the_rects_asked_for) {
    fixture* f = gpu();
    if (!f)
        return;
    const lx::rect2i rects[2] = {{0, 0, 8, 4}, {40, 30, 10, 10}};
    LUMEN_CHECK(static_cast<bool>(f->vk.read_back_async(5, f->target, rects, 2)));
    const unsigned char* rows = f->vk.read_back_wait(5, 2'000'000'000ull);
    LUMEN_CHECK(rows != nullptr);
    if (rows) {
        // B, G, R, A at each rect's own place: red on the left, blue on the right.
        const unsigned char* a = rows + (2u * k_w + 3u) * 4u;
        const unsigned char* b = rows + (35u * k_w + 45u) * 4u;
        LUMEN_CHECK(a[2] > 200 && a[0] < 60);
        LUMEN_CHECK(b[0] > 200 && b[2] < 60);
    }
    // Reused at once (the previous read waited for).
    LUMEN_CHECK(static_cast<bool>(f->vk.read_back_async(5, f->target, nullptr, 0)));
    LUMEN_CHECK(f->vk.read_back_wait(5, 2'000'000'000ull) != nullptr);
    f->vk.read_back_free(5);
    LUMEN_CHECK(f->vk.read_back_wait(5, 0) == nullptr);
}

// ── the synchronous debug read-back is pooled ───────────────────────────────

LUMEN_TEST(vk_read_back_latency_at_panel_resolution) {
    // One number for the evidence file (AGENTS.md §1.2): the read-back a screencast frame
    // pays at this host's panel size, GPU copy → fence → rows visible. Not a gate — printed,
    // not asserted (host frequency states move it; see the build-run skill).
    fixture* f = gpu();
    if (!f)
        return;
    constexpr unsigned k_pw = 3200;
    constexpr unsigned k_ph = 1800;
    auto t = lx::gfx::render_target::create(f->device.context(), k_pw, k_ph);
    if (!t)
        return;
    lx::gfx::render_target panel = std::move(t).value();
    std::vector<unsigned char> rgba(k_pw * k_ph * 4u, 255);
    (void)f->vk.upload_rgba(0x4000'0303u, k_pw, k_ph, rgba.data());
    lx::gfx::blit_command cmd{};
    cmd.texture_id = 0x4000'0303u;
    cmd.dst = {0, 0, static_cast<int>(k_pw), static_cast<int>(k_ph)};
    auto c = f->vk.composite(panel, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    if (c && c.value().out_fence_fd >= 0)
        ::close(c.value().out_fence_fd);
    if (!c)
        return;
    double samples[16];
    unsigned n = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!f->vk.read_back_async(63, panel, nullptr, 0))
            return;
        (void)f->vk.read_back_wait(63, 2'000'000'000ull);
        samples[n++] = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
    }
    std::sort(samples, samples + n);
    const double median = (samples[n / 2 - 1] + samples[n / 2]) / 2.0;
    std::printf("  read-back %ux%u: %.2f ms median (min %.2f, max %.2f, %u reads)\n", k_pw,
                k_ph, median, samples[0], samples[n - 1], n);
    LUMEN_CHECK(median > 0.0 && median < 250.0); // sane, not a gate
}

LUMEN_TEST(vk_sync_read_back_reuses_its_buffer) {
    fixture* f = gpu();
    if (!f)
        return;
    std::vector<unsigned char> a(k_w * k_h * 4u, 0);
    LUMEN_CHECK(static_cast<bool>(f->vk.read_back(f->target, a.data(),
                                                  static_cast<unsigned>(a.size()))));
    const unsigned made = f->vk.readback_allocations();
    LUMEN_CHECK(made >= 1);
    for (unsigned i = 0; i < 3; ++i) {
        std::vector<unsigned char> again(k_w * k_h * 4u, 0);
        LUMEN_CHECK(static_cast<bool>(f->vk.read_back(f->target, again.data(),
                                                      static_cast<unsigned>(again.size()))));
        // R, G, B, A rows: red on the left, blue on the right, every time.
        const unsigned char* left = &again[(20 * k_w + 10) * 4u];
        const unsigned char* right = &again[(20 * k_w + 50) * 4u];
        LUMEN_CHECK(left[0] > 200 && left[2] < 60);
        LUMEN_CHECK(right[2] > 200 && right[0] < 60);
    }
    // A same-size read-back allocates nothing (the SIGUSR1 path: on demand, pooled).
    LUMEN_CHECK(f->vk.readback_allocations() == made);
}

#ifdef LUMEN_HAS_EGL

// ── capture dma-buf: the GPU places the rows at the client's stride ─────────

LUMEN_TEST(vk_output_frame_is_copied_into_a_client_dmabuf) {
    fixture* f = gpu();
    if (!f || !f->dmabufs)
        return;
    // 62 wide: GBM's LINEAR stride is padded past 62*4 — the placement under test.
    client_dmabuf client{f->gbm, 62, 47};
    LUMEN_CHECK(client.bo != nullptr && client.fd >= 0);
    if (!client.bo)
        return;
    client.fill(0x5A);
    lx::gfx::render_target target = red_blue_target(*f, 62, 47);
    LUMEN_CHECK(target.valid());
    if (!target.valid())
        return;
    auto imported = f->vk.capture_dmabuf_import(7, client.fd, 0, client.stride, 62, 47, k_xr24,
                                                0);
    LUMEN_CHECK(static_cast<bool>(imported));
    if (!imported)
        return;
    const unsigned made = f->vk.capture_dmabuf_imports();
    for (unsigned frame = 0; frame < 3; ++frame) {
        auto copy = f->vk.capture_dmabuf_copy(9, 7, target, nullptr, 0, 0, 0);
        LUMEN_CHECK(static_cast<bool>(copy));
        if (!copy)
            return;
        wait_fence(copy.value());
        // B, G, R at the client's stride: red left, blue right.
        const std::uint32_t red = client.at(5, 23);
        const std::uint32_t blue = client.at(50, 23);
        LUMEN_CHECK((red & 0xFF) < 60 && ((red >> 16) & 0xFF) > 200);
        LUMEN_CHECK((blue & 0xFF) > 200 && ((blue >> 16) & 0xFF) < 60);
    }
    // The buffer of one screencast imports once; its frames copy without allocating.
    LUMEN_CHECK(f->vk.capture_dmabuf_imports() == made);
    f->vk.capture_dmabuf_forget(7);
}

LUMEN_TEST(vk_dmabuf_copy_copies_only_the_rects_asked_for) {
    fixture* f = gpu();
    if (!f || !f->dmabufs)
        return;
    client_dmabuf client{f->gbm, 62, 47};
    LUMEN_CHECK(client.bo != nullptr && client.fd >= 0);
    if (!client.bo)
        return;
    client.fill(0x5A);
    lx::gfx::render_target target = red_blue_target(*f, 62, 47);
    LUMEN_CHECK(target.valid());
    if (!target.valid())
        return;
    LUMEN_CHECK(static_cast<bool>(f->vk.capture_dmabuf_import(8, client.fd, 0, client.stride,
                                                              62, 47, k_xr24, 0)));
    // The left half only (source pixels — no crop).
    const lx::rect2i rects[1] = {{0, 0, 31, 47}};
    auto copy = f->vk.capture_dmabuf_copy(10, 8, target, rects, 1, 0, 0);
    LUMEN_CHECK(static_cast<bool>(copy));
    if (!copy)
        return;
    wait_fence(copy.value());
    // Red inside the rect, the fill pattern outside it (columns 31..61 keep 0x5A5A5A).
    const std::uint32_t red = client.at(5, 23);
    const std::uint32_t untouched = client.at(45, 23);
    LUMEN_CHECK(((red >> 16) & 0xFF) > 200 && (red & 0xFF) < 60);
    LUMEN_CHECK((untouched & 0xFFFFFF) == 0x5A5A5Au);
    f->vk.capture_dmabuf_forget(8);
}

LUMEN_TEST(vk_dmabuf_curtain_fills_opaque_black) {
    fixture* f = gpu();
    if (!f || !f->dmabufs)
        return;
    client_dmabuf client{f->gbm, 62, 47};
    LUMEN_CHECK(client.bo != nullptr && client.fd >= 0);
    if (!client.bo)
        return;
    client.fill(0x77);
    LUMEN_CHECK(static_cast<bool>(f->vk.capture_dmabuf_import(9, client.fd, 0, client.stride,
                                                              62, 47, k_xr24, 0)));
    auto filled = f->vk.capture_dmabuf_fill(11, 9);
    LUMEN_CHECK(static_cast<bool>(filled));
    if (!filled)
        return;
    wait_fence(filled.value());
    const std::uint32_t px = client.at(30, 20);
    LUMEN_CHECK((px & 0xFFFFFF) == 0);
    // The padding byte is opaque (a reader that takes it as alpha must see the curtain).
    std::uint32_t s = 0;
    void* data = nullptr;
    void* p = gbm_bo_map(client.bo, 0, 0, 1, 1, GBM_BO_TRANSFER_READ, &s, &data);
    LUMEN_CHECK(p && (*static_cast<const std::uint32_t*>(p) & 0xFF000000u) == 0xFF000000u);
    if (p)
        gbm_bo_unmap(client.bo, data);
    f->vk.capture_dmabuf_forget(9);
}

// ── window capture through the engine ───────────────────────────────────────

LUMEN_TEST(vk_window_view_is_composited_and_read_back_into_shm) {
    fixture* f = gpu();
    if (!f)
        return;
    rig r{*f};
    std::vector<unsigned char> mem(k_w * k_h * 4u, 0x11);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::toplevel;
    j.session = 4;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.view = 0x100; // any capture-view id: the engine matches it against the job's own
    j.view_frame = 0;
    j.target.width = k_w;
    j.target.height = k_h;
    j.target.shm = r.guards.add(mem.data(), mem.data(), mem.data(), k_w * 4u, k_w, k_h, true);
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    // The view's single draw, as `capture_draws` would hand it.
    lx::gfx::blit_command draw{};
    draw.texture_id = 0x4000'0101u;
    draw.dst = {0, 0, static_cast<int>(k_w), static_cast<int>(k_h)};
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
    src.blits = &draw;
    src.blit_count = 1;
    src.clear = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    lx::gfx::damage_region all{};
    all.set_full();
    r.renderer.toplevel_composited(j.view, all, src, r.channel);
    lx::compositor::capture_completion c[4];
    LUMEN_CHECK(r.channel.drain(c, 4) == 1 && c[0].result == lx::compositor::capture_result::ready);
    const unsigned char* left = &mem[(20 * k_w + 10) * 4u];
    const unsigned char* right = &mem[(20 * k_w + 50) * 4u];
    LUMEN_CHECK(left[2] > 200 && left[0] < 60);
    LUMEN_CHECK(right[0] > 200 && right[2] < 60);
}

LUMEN_TEST(vk_window_view_is_copied_into_a_client_dmabuf) {
    fixture* f = gpu();
    if (!f || !f->dmabufs)
        return;
    client_dmabuf client{f->gbm, 62, 47};
    LUMEN_CHECK(client.bo != nullptr && client.fd >= 0);
    if (!client.bo)
        return;
    client.fill(0x5A);
    // The window's draw is the 62x47 texture: made for this size by `red_blue_target`'s
    // upload — a second one here keeps the fixture's own untouched.
    lx::gfx::render_target reference = red_blue_target(*f, 62, 47);
    LUMEN_CHECK(reference.valid());
    rig r{*f};
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::toplevel;
    j.session = 5;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.view = 0x101;
    j.view_frame = 0;
    j.target.dmabuf = true;
    j.target.width = 62;
    j.target.height = 47;
    j.target.buffer_key = 42;
    j.target.fd = ::fcntl(client.fd, F_DUPFD_CLOEXEC, 0);
    j.target.stride = client.stride;
    j.target.format = k_xr24;
    LUMEN_CHECK(j.target.fd >= 0);
    lx::gfx::blit_command draw{};
    draw.texture_id = 0x4000'0202u;
    draw.dst = {0, 0, 62, 47};
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {62, 47};
    src.blits = &draw;
    src.blit_count = 1;
    src.clear = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    lx::gfx::damage_region all{};
    all.set_full();
    r.renderer.toplevel_composited(j.view, all, src, r.channel);
    lx::compositor::capture_completion c[4];
    const unsigned n = r.channel.drain(c, 4);
    LUMEN_CHECK(n == 1 && (c[0].result == lx::compositor::capture_result::ready ||
                           c[0].result == lx::compositor::capture_result::fence));
    if (n == 1) {
        if (c[0].fence_fd >= 0) {
            pollfd p{c[0].fence_fd, POLLIN, 0};
            (void)::poll(&p, 1, 2000);
            ::close(c[0].fence_fd);
        }
        const std::uint32_t red = client.at(5, 23);
        const std::uint32_t blue = client.at(50, 23);
        LUMEN_CHECK(((red >> 16) & 0xFF) > 200 && (red & 0xFF) < 60);
        LUMEN_CHECK((blue & 0xFF) > 200 && ((blue >> 16) & 0xFF) < 60);
    }
    // The session's end frees its slot and target.
    lx::compositor::capture_job forget{};
    forget.kind = lx::compositor::capture_job_kind::forget_session;
    forget.session = 5;
    (void)r.channel.post(forget);
    r.renderer.take(r.channel);
}

LUMEN_TEST(vk_take_sample_fence_never_hands_on_a_closed_fd) {
    // The Z7 flake's first domino: with release points unwanted, the sample fence was
    // closed AND its number returned — the caller published the dead number into the
    // release mailbox, whose later replace-close of it killed whatever had reused the
    // number (a fresh client's accepted socket). Unwanted now: consumed inside, -1 out.
    // Wanted: a live fd the caller owns.
    fixture* f = gpu();
    if (!f)
        return;
    lx::gfx::render_target reference = red_blue_target(*f, 62, 47);
    LUMEN_CHECK(reference.valid());
    rig r{*f};
    lx::gfx::blit_command draw{};
    draw.texture_id = 0x4000'0202u;
    draw.dst = {0, 0, 62, 47};
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {62, 47};
    src.blits = &draw;
    src.blit_count = 1;
    src.clear = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    lx::gfx::damage_region all{};
    all.set_full();
    const auto composite_once = [&]() {
        lx::compositor::capture_job j{};
        j.kind = lx::compositor::capture_job_kind::toplevel;
        j.session = 7;
        j.generation = 1;
        j.serial = 1;
        j.full = true;
        j.view = 0x101;
        j.view_frame = 0;
        j.target.width = 62; // the view composite runs at the buffer's size; no copy-out
        j.target.height = 47;
        (void)r.channel.post(j);
        r.renderer.take(r.channel);
        r.renderer.toplevel_composited(j.view, all, src, r.channel);
        lx::compositor::capture_completion c[4];
        const unsigned n = r.channel.drain(c, 4);
        if (n == 1 && c[0].fence_fd >= 0)
            ::close(c[0].fence_fd);
    };
    composite_once();
    composite_once();
    // Unwanted: consumed inside — never a closed number handed on, and nothing is left
    // stored for a later take to publish either. (The wanted half — a live, owned fd —
    // is exercised at 60 fps by every seat smoke: the release-fence imports of the
    // composite path would refuse a dead one.)
    LUMEN_CHECK(r.renderer.take_sample_fence(false) == -1);
    LUMEN_CHECK(r.renderer.take_sample_fence(true) == -1);
    lx::compositor::capture_job forget{};
    forget.kind = lx::compositor::capture_job_kind::forget_session;
    forget.session = 7;
    (void)r.channel.post(forget);
    r.renderer.take(r.channel);
}

LUMEN_TEST(vk_engine_output_job_copies_into_a_client_dmabuf) {
    fixture* f = gpu();
    if (!f || !f->dmabufs)
        return;
    client_dmabuf client{f->gbm, k_w, k_h};
    LUMEN_CHECK(client.bo != nullptr && client.fd >= 0);
    if (!client.bo)
        return;
    client.fill(0x5A);
    rig r{*f};
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 6;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.target.dmabuf = true;
    j.target.width = k_w;
    j.target.height = k_h;
    j.target.buffer_key = 43;
    j.target.fd = ::fcntl(client.fd, F_DUPFD_CLOEXEC, 0);
    j.target.stride = client.stride;
    j.target.format = k_xr24;
    LUMEN_CHECK(j.target.fd >= 0);
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
    src.vk_target = &f->target;
    r.renderer.output_front(0, src, r.channel);
    lx::compositor::capture_completion c[4];
    const unsigned n = r.channel.drain(c, 4);
    LUMEN_CHECK(n == 1 && (c[0].result == lx::compositor::capture_result::ready ||
                           c[0].result == lx::compositor::capture_result::fence));
    if (n == 1) {
        if (c[0].fence_fd >= 0) {
            pollfd p{c[0].fence_fd, POLLIN, 0};
            (void)::poll(&p, 1, 2000);
            ::close(c[0].fence_fd);
        }
        const std::uint32_t red = client.at(10, 20);
        const std::uint32_t blue = client.at(50, 20);
        LUMEN_CHECK(((red >> 16) & 0xFF) > 200 && (red & 0xFF) < 60);
        LUMEN_CHECK((blue & 0xFF) > 200 && ((blue >> 16) & 0xFF) < 60);
    }
    lx::compositor::capture_job forget{};
    forget.kind = lx::compositor::capture_job_kind::forget_session;
    forget.session = 6;
    (void)r.channel.post(forget);
    r.renderer.take(r.channel);
    lx::compositor::capture_job drop{};
    drop.kind = lx::compositor::capture_job_kind::forget_buffer;
    drop.target.buffer_key = 43;
    (void)r.channel.post(drop);
    r.renderer.take(r.channel);
}

LUMEN_TEST(vk_engine_dmabuf_job_with_cursor_lands) {
    // The dmabuf read worker probed the slot's mapped rows (`read_back_wait`) — but a
    // dma-buf copy's slot maps none, so every paint_cursors frame over a dma-buf was
    // reported failed after the copy had landed in the client's buffer. The wait is the
    // slot's fence; the frame must complete and the pointer must land in the buffer.
    fixture* f = gpu();
    if (!f || !f->dmabufs)
        return;
    client_dmabuf client{f->gbm, k_w, k_h};
    LUMEN_CHECK(client.bo != nullptr && client.fd >= 0);
    if (!client.bo)
        return;
    client.fill(0x5A);
    rig r{*f};
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 8;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.paint_cursors = true;
    j.target.dmabuf = true;
    j.target.width = k_w;
    j.target.height = k_h;
    j.target.buffer_key = 44;
    j.target.fd = ::fcntl(client.fd, F_DUPFD_CLOEXEC, 0);
    j.target.stride = client.stride;
    j.target.format = k_xr24;
    LUMEN_CHECK(j.target.fd >= 0);
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    std::vector<std::uint32_t> pointer(4u * 4u, 0xFFFF'FFFFu); // opaque white, 4x4
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
    src.vk_target = &f->target;
    src.cursor.pixels = pointer.data();
    src.cursor.width = 4;
    src.cursor.height = 4;
    src.cursor.stride_px = 4;
    src.cursor.rect = {10, 20, 4, 4};
    src.cursor.visible = true;
    r.renderer.output_front(0, src, r.channel);
    lx::compositor::capture_completion c[4];
    const unsigned n = r.channel.drain(c, 4);
    LUMEN_CHECK(n == 1 && (c[0].result == lx::compositor::capture_result::ready ||
                           c[0].result == lx::compositor::capture_result::fence));
    if (n == 1) {
        wait_fence(c[0].fence_fd);
        const std::uint32_t red = client.at(5, 23);
        const std::uint32_t pointed = client.at(12, 22);
        LUMEN_CHECK(((red >> 16) & 0xFF) > 200 && (red & 0xFF) < 60);
        LUMEN_CHECK(pointed == 0x00FF'FFFFu); // the blended pointer, opaque white
    }
    lx::compositor::capture_job forget{};
    forget.kind = lx::compositor::capture_job_kind::forget_session;
    forget.session = 8;
    (void)r.channel.post(forget);
    r.renderer.take(r.channel);
    lx::compositor::capture_job drop{};
    drop.kind = lx::compositor::capture_job_kind::forget_buffer;
    drop.target.buffer_key = 44;
    (void)r.channel.post(drop);
    r.renderer.take(r.channel);
}

#endif // LUMEN_HAS_EGL

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
