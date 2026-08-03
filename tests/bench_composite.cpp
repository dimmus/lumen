// Measures the CPU composite path against the per-frame passes it replaces.
//
// The GPU path on a software-rasterizer host makes several passes over every frame: a
// channel swizzle at commit, a memcpy into the Vulkan staging ring, a tiling copy into the
// sampled image, and a fullscreen fragment shader into the scanout target. The CPU path
// makes one. This prints the cost of each so the difference is a number rather than a
// claim — run it on the machine in question.
//
//   bench_composite [width height]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

import lx.foundation;
import lx.gfx;

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
constexpr auto k_rgba = static_cast<lx::fourcc>(lx::pixel_format::rgba8888);

/// Keeps the optimiser from deleting work whose result is otherwise unused.
volatile std::uint32_t sink = 0;

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/// The swizzle the Vulkan ingest path runs on every commit, for comparison.
void convert_to_rgba(const std::uint32_t* src, std::uint32_t* dst, std::size_t pixels) {
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t p = src[i];
        dst[i] = ((p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16)) |
                 0xFF000000u;
    }
}

lx::gfx::pixel_surface surface_of(std::vector<std::uint32_t>& pixels, unsigned w, unsigned h,
                                  lx::fourcc format) {
    lx::gfx::pixel_surface s{};
    s.pixels = reinterpret_cast<unsigned char*>(pixels.data());
    s.width = w;
    s.height = h;
    s.stride = w * 4u;
    s.format = format;
    return s;
}

double bench_composite(unsigned width, unsigned height, lx::fourcc src_format, unsigned workers,
                       int iters) {
    std::vector<std::uint32_t> src(static_cast<std::size_t>(width) * height, 0xFF204080u);
    std::vector<std::uint32_t> dst(static_cast<std::size_t>(width) * height, 0u);

    lx::gfx::cpu_compositor comp;
    comp.set_worker_threads(workers);
    auto src_surface = surface_of(src, width, height, src_format);
    if (!comp.register_texture(1, src_surface))
        return -1.0;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = 1;
    cmd.dst = {0, 0, static_cast<int>(width), static_cast<int>(height)};
    cmd.blend = lx::blend_mode::opaque;
    cmd.opacity = 1.f;

    auto dst_surface = surface_of(dst, width, height, k_xrgb);
    // Warm the caches so the first frame's page faults are not counted.
    (void)comp.composite(dst_surface, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        auto r = comp.composite(dst_surface, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
        if (!r)
            return -1.0;
    }
    return ms_since(t0) / iters;
}

/// The GL backend against a real render node. Prints nothing when there is no usable GPU.
void bench_gl(unsigned width, unsigned height, int iters) {
    const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;
    auto device = lx::gfx::egl_device::create(fd);
    if (!device) {
        ::close(fd);
        return;
    }
    if (device.value().is_software_renderer()) {
        ::close(fd);
        return;
    }

    auto& dev = device.value();
    auto target = lx::gfx::gl_scanout_target::create(dev, width, height, k_xrgb);
    lx::gfx::gl_compositor comp;
    if (!target || !comp.initialize(dev)) {
        ::close(fd);
        return;
    }

    std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4, 0x40);
    if (!comp.upload_rgba(1, width, height, px.data())) {
        ::close(fd);
        return;
    }

    lx::gfx::blit_command cmd{};
    cmd.texture_id = 1;
    cmd.dst = {0, 0, static_cast<int>(width), static_cast<int>(height)};
    cmd.blend = lx::blend_mode::opaque;
    cmd.opacity = 1.f;

    std::printf("  -- GL backend (%s) --\n", dev.renderer());

    // `composite` returns an owned sync_file; the compositor hands it to the atomic commit,
    // and a benchmark that ignored it would run out of descriptors.
    const auto draw = [&] {
        auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
        if (stats && stats.value().out_fence_fd >= 0)
            ::close(stats.value().out_fence_fd);
    };

    std::printf("  fence: %s\n",
                dev.supports_native_fence() ? "EGL_ANDROID_native_fence_sync (no CPU stall)"
                                            : "unavailable — composite blocks on glFinish");

    // Steady state for a dma-buf client: the buffer is already on the GPU, so a window
    // costs one textured quad and no upload at all.
    draw();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        draw();
    // With a fence in play this is *render-thread* cost, not frame time: the GPU work is
    // submitted and overlaps, and the KMS flip waits on the fence instead of the CPU. It is
    // the number that matters for the tick budget, not a claim about GPU throughput.
    std::printf("  composite, dmabuf client (no upload)       %7.3f ms  (render thread)\n",
                ms_since(t0) / iters);

    // Steady state for an shm client: every frame has to be uploaded first.
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        (void)comp.upload_rgba(1, width, height, px.data());
        draw();
    }
    std::printf("  composite, shm client (full upload)        %7.3f ms  (render thread)\n",
                ms_since(t0) / iters);

    comp.shutdown();
    ::close(fd);
}

} // namespace

int main(int argc, char** argv) {
    unsigned width = 1918;
    unsigned height = 928;
    if (argc >= 3) {
        const int w = std::atoi(argv[1]);
        const int h = std::atoi(argv[2]);
        if (w > 0 && h > 0) {
            width = static_cast<unsigned>(w);
            height = static_cast<unsigned>(h);
        }
    }

    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    const double mib = static_cast<double>(pixels * 4) / (1024.0 * 1024.0);
    constexpr int k_iters = 60;

    std::printf("bench_composite: %ux%u (%.1f MiB/frame), %d frames per case\n", width, height,
                mib, k_iters);

    // What the old ingest cost before the composite even started.
    {
        std::vector<std::uint32_t> src(pixels, 0xFF204080u);
        std::vector<std::uint32_t> dst(pixels, 0u);
        convert_to_rgba(src.data(), dst.data(), pixels);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < k_iters; ++i) {
            src[static_cast<std::size_t>(i) % pixels] = 0xFF000000u | static_cast<unsigned>(i);
            convert_to_rgba(src.data(), dst.data(), pixels);
            sink += dst[static_cast<std::size_t>(i) % pixels];
        }
        std::printf("  shm ingest, swizzle to RGBA (Vulkan path)   %7.3f ms\n",
                    ms_since(t0) / k_iters);
    }

    {
        std::vector<std::uint32_t> src(pixels, 0xFF204080u);
        std::vector<std::uint32_t> dst(pixels, 0u);
        std::memcpy(dst.data(), src.data(), pixels * 4);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < k_iters; ++i) {
            // Vary a source pixel per iteration and read the result back: without this the
            // optimiser is free to hoist the whole copy out of the loop and the case times
            // at 0 ms.
            src[static_cast<std::size_t>(i) % pixels] = 0xFF000000u | static_cast<unsigned>(i);
            std::memcpy(dst.data(), src.data(), pixels * 4);
            sink += dst[static_cast<std::size_t>(i) % pixels];
        }
        std::printf("  shm ingest, native memcpy (CPU path)        %7.3f ms\n",
                    ms_since(t0) / k_iters);
    }

    std::printf("  composite, opaque fullscreen, 1 thread      %7.3f ms\n",
                bench_composite(width, height, k_xrgb, 0, k_iters));
    std::printf("  composite, opaque fullscreen, 4 threads     %7.3f ms\n",
                bench_composite(width, height, k_xrgb, 3, k_iters));
    std::printf("  composite, channel swap, 1 thread           %7.3f ms\n",
                bench_composite(width, height, k_rgba, 0, k_iters));
    std::printf("  composite, channel swap, 4 threads          %7.3f ms\n",
                bench_composite(width, height, k_rgba, 3, k_iters));

    bench_gl(width, height, k_iters);

    // Never fail on timing — CTest only checks exit status.
    return 0;
}
