// Measures the CPU composite path against the per-frame passes it replaces.
//
// The GPU path on a software-rasterizer host makes several passes over every frame: a
// channel swizzle at commit, a memcpy into the Vulkan staging ring, a tiling copy into the
// sampled image, and a fullscreen fragment shader into the scanout target. The CPU path
// makes one. This prints the cost of each so the difference is a number rather than a
// claim — run it on the machine in question.
//
//   bench_composite [width height] [--vk] [--gpu-timing] [--hash] [--animate] [--downsample]
//
// --vk: the Vulkan cases on a software rasterizer too (lavapipe takes ~0.1 s a frame at the
// default size, too slow for every CTest run); a hardware Vulkan device always runs them.
// --gpu-timing: the Vulkan cases time each frame on the GPU (`set_gpu_timing`: timestamps
// around the composite and encode passes) and append the mean; the other numbers are the
// timing's overhead when compared with a run without it.
// --hash: each scene's composited frame is read back and its FNV-1a hash printed per
// backend, so two builds can be compared pixel for pixel (no timing in it).
// --downsample: a fullscreen draw of a buffer 1, 2, 4/3 and 8/5 times the output's size (a
// render scale over the output scale: rendering-performance.md "Render scale") on each
// backend — what the area filter costs over one bilinear tap.
// --animate: the top window of each scene is drawn as one frame of a window animation
// ([animations]: its tree container's `node_animation`, 0.95 of its size about its centre
// at opacity 0.6): what an animating frame costs every backend.

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
constexpr auto k_rgba = static_cast<lx::fourcc>(lx::pixel_format::rgba8888);

/// Keeps the optimiser from deleting work whose result is otherwise unused.
volatile std::uint32_t sink = 0;

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/// --hash: print each scene's frame hash per backend.
bool g_hash = false;
/// --animate: the top window of each scene mid-animation.
bool g_animate = false;

/// FNV-1a over `n` bytes.
std::uint64_t fnv1a(const unsigned char* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/// --gpu-timing.
bool g_gpu_timing = false;

/// GPU time of a Vulkan case's frames (`vulkan_compositor::take_gpu_times`), summed.
struct gpu_times {
    double composite_ms = 0.0;
    double encode_ms = 0.0;
    unsigned frames = 0;
    void take(lx::gfx::vulkan_compositor& comp) {
        lx::gfx::composite_gpu_time t[lx::gfx::vulkan_compositor::k_gpu_times]{};
        const unsigned n = comp.take_gpu_times(t, lx::gfx::vulkan_compositor::k_gpu_times);
        for (unsigned i = 0; i < n; ++i) {
            composite_ms += static_cast<double>(t[i].composite_ns) / 1e6;
            encode_ms += static_cast<double>(t[i].encode_ns) / 1e6;
            ++frames;
        }
    }
    /// Ends the line: the mean per frame with --gpu-timing, nothing more without.
    void print() const {
        if (g_gpu_timing && frames > 0)
            std::printf("  gpu %.3f ms (composite %.3f, encode %.3f; %u frames)",
                        (composite_ms + encode_ms) / frames, composite_ms / frames,
                        encode_ms / frames, frames);
        else if (g_gpu_timing)
            std::printf("  gpu n/a");
        std::printf("\n");
    }
};

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

/// One Vulkan compositor drawing alternately into two scanout targets — two outputs sharing
/// the render thread's `vulkan_compositor`. Same size is the double-buffered case; two sizes
/// is two outputs of different modes. Per-composite wall time with the frame's fence waited
/// (on lavapipe the GPU work is CPU work, so this is the whole frame).
void bench_vk(unsigned width, unsigned height, int iters, bool on_software) {
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected)
        return;
    auto device = std::move(selected).value();
    if (device.info().is_software_rasterizer && !on_software)
        return;
    lx::gfx::vulkan_compositor comp;
    if (!comp.initialize(device.context()))
        return;
    comp.set_gpu_timing(g_gpu_timing);
    std::vector<unsigned char> px(256u * 256u * 4u, 0x40);
    if (!comp.upload_rgba(1, 256, 256, px.data()))
        return;
    std::printf("  -- Vulkan backend (%s%s) --\n", device.info().device_name,
                device.info().is_software_rasterizer ? ", software" : "");

    // All four live for the whole run: one framebuffer each in the compositor's cache.
    const unsigned w2 = width * 2u / 3u;
    const unsigned h2 = height * 2u / 3u;
    auto a0 = lx::gfx::render_target::create(device.context(), width, height);
    auto a1 = lx::gfx::render_target::create(device.context(), width, height);
    auto b0 = lx::gfx::render_target::create(device.context(), w2, h2);
    auto b1 = lx::gfx::render_target::create(device.context(), w2, h2);
    if (!a0 || !a1 || !b0 || !b1)
        return;
    const auto run = [&](const char* label, lx::gfx::render_target& x,
                         lx::gfx::render_target& y) {
        lx::gfx::render_target* targets[2] = {&x, &y};
        double call_ms = 0.0;
        gpu_times gpu{};
        const auto draw = [&](int i) {
            lx::gfx::render_target& t = *targets[i & 1];
            lx::gfx::blit_command cmd{};
            cmd.texture_id = 1;
            cmd.dst = {0, 0, static_cast<int>(t.width()), static_cast<int>(t.height())};
            cmd.blend = lx::blend_mode::opaque;
            cmd.opacity = 1.f;
            const auto c0 = std::chrono::steady_clock::now();
            auto stats = comp.composite(t, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
            call_ms += ms_since(c0);
            if (stats && stats.value().out_fence_fd >= 0) {
                pollfd pfd{stats.value().out_fence_fd, POLLIN, 0};
                (void)::poll(&pfd, 1, 1000);
                ::close(stats.value().out_fence_fd);
            }
            if (g_gpu_timing)
                gpu.take(comp);
        };
        draw(0);
        draw(1);
        call_ms = 0.0;
        gpu = {};
        rusage r0{};
        ::getrusage(RUSAGE_SELF, &r0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i)
            draw(i);
        const double wall = ms_since(t0) / iters;
        rusage r1{};
        ::getrusage(RUSAGE_SELF, &r1);
        const auto cpu_us = [](const rusage& r) {
            return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e6 + r.ru_utime.tv_usec +
                   r.ru_stime.tv_usec;
        };
        // Render-thread time is the composite call (set-up, record, submit); wall adds the
        // fence wait. Faults: a reallocated intermediate is faulted in again by its clear.
        if (g_gpu_timing)
            gpu.take(comp);
        std::printf("  %-42s %7.3f ms  (call %.3f ms, cpu %.1f ms, %ld faults)", label, wall,
                    call_ms / iters, (cpu_us(r1) - cpu_us(r0)) / 1e3 / iters,
                    (r1.ru_minflt - r0.ru_minflt) / iters);
        gpu.print();
    };
    run("composite, 2 targets same size", a0.value(), a1.value());
    run("composite, 2 targets same size (2/3)", b0.value(), b1.value());
    run("composite, 2 targets alternating sizes", a0.value(), b0.value());
    comp.shutdown();
}

/// One output's frames on Vulkan, as the compositor draws them: three rotating scanout
/// targets, each frame's region its damage plus the slot's age (buffer age). A desktop:
/// a scaled wallpaper, an opaque window, a translucent one over it, a cursor. Damage is a
/// moving cursor, one changed terminal line, or everything. `composite_region` with the
/// region and the previous slot (only the damage composited into the intermediate), and
/// for reference `composite` of the whole target, what every frame cost before.
void bench_vk_damage(unsigned width, unsigned height, int iters, bool on_software) {
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected)
        return;
    auto device = std::move(selected).value();
    if (device.info().is_software_rasterizer && !on_software)
        return;
    lx::gfx::vulkan_compositor comp;
    if (!comp.initialize(device.context()))
        return;
    comp.set_gpu_timing(g_gpu_timing);
    std::vector<unsigned char> px(256u * 256u * 4u, 0x60);
    for (std::size_t i = 3; i < px.size(); i += 4)
        px[i] = 0xFF;
    std::vector<unsigned char> tr(256u * 256u * 4u, 0x40); // premultiplied, alpha 0x80
    for (std::size_t i = 3; i < tr.size(); i += 4)
        tr[i] = 0x80;
    if (!comp.upload_rgba(1, 256, 256, px.data()) || !comp.upload_rgba(2, 256, 256, px.data()) ||
        !comp.upload_rgba(3, 256, 256, tr.data()) || !comp.upload_rgba(4, 64, 64, tr.data()))
        return;
    lx::gfx::render_target targets[3];
    for (auto& t : targets) {
        auto made = lx::gfx::render_target::create(device.context(), width, height);
        if (!made)
            return;
        t = std::move(made).value();
    }
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    lx::gfx::blit_command cmds[4]{};
    cmds[0].texture_id = 1;
    cmds[0].dst = {0, 0, w, h};
    cmds[0].blend = lx::blend_mode::opaque;
    cmds[0].scaled = true;
    cmds[1].texture_id = 2;
    cmds[1].dst = {w / 10, h / 10, w / 2, h / 2};
    cmds[1].blend = lx::blend_mode::opaque;
    cmds[1].scaled = true;
    cmds[2].texture_id = 3;
    cmds[2].dst = {w / 3, h / 3, w / 2, h / 2};
    cmds[2].scaled = true;
    cmds[3].texture_id = 4;
    cmds[3].dst = {w / 2, h / 2, 64, 64};
    std::printf("  -- Vulkan damage (%ux%u, 3 slots, buffer age) --\n", width, height);

    enum class kind { full, cursor, line };
    const auto run = [&](const char* label, kind k, bool whole) {
        const lx::gfx::render_target* previous = nullptr;
        lx::gfx::damage_region age[3]{};
        for (auto& a : age)
            a.set_full();
        unsigned long long area = 0;
        gpu_times gpu{};
        const auto frame = [&](int i) {
            const unsigned back = static_cast<unsigned>(i) % 3u;
            lx::gfx::damage_region changed{};
            if (k == kind::full) {
                changed.set_full();
            } else if (k == kind::cursor) {
                const lx::rect2i old = cmds[3].dst;
                cmds[3].dst.x = w / 4 + (i * 7) % (w / 2);
                cmds[3].dst.y = h / 4 + (i * 3) % (h / 2);
                changed.add(old);
                changed.add(cmds[3].dst);
            } else {
                changed.add({cmds[1].dst.x + 8, cmds[1].dst.y + 40 + (i % 20) * 18,
                             cmds[1].dst.width - 16, 18});
            }
            changed.clip({0, 0, w, h});
            lx::gfx::damage_region region = changed;
            region.add_region(age[back]);
            region.clip({0, 0, w, h});
            area += region.area();
            auto stats = whole ? comp.composite(targets[back], lx::color::rgb(0.f, 0.f, 0.f),
                                                cmds, 4)
                               : comp.composite_region(targets[back], lx::color::rgb(0.f, 0.f, 0.f),
                                                       cmds, 4, region, changed, previous);
            previous = &targets[back];
            if (stats && stats.value().out_fence_fd >= 0) {
                pollfd pfd{stats.value().out_fence_fd, POLLIN, 0};
                (void)::poll(&pfd, 1, 1000);
                ::close(stats.value().out_fence_fd);
            }
            if (g_gpu_timing)
                gpu.take(comp);
            for (unsigned s = 0; s < 3; ++s)
                if (s != back)
                    age[s].add_region(changed);
            age[back].clear();
        };
        for (int i = 0; i < 3; ++i)
            frame(i);
        area = 0;
        gpu = {};
        rusage r0{};
        ::getrusage(RUSAGE_SELF, &r0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i)
            frame(i + 3);
        const double wall = ms_since(t0) / iters;
        rusage r1{};
        ::getrusage(RUSAGE_SELF, &r1);
        const auto cpu_us = [](const rusage& r) {
            return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e6 + r.ru_utime.tv_usec +
                   r.ru_stime.tv_usec;
        };
        if (g_gpu_timing)
            gpu.take(comp);
        std::printf("  %-42s %7.3f ms  (cpu %.2f ms, region %llu px)", label, wall,
                    (cpu_us(r1) - cpu_us(r0)) / 1e3 / iters, area / static_cast<unsigned>(iters));
        gpu.print();
    };
    run("whole target (composite), full damage", kind::full, true);
    run("frame, full damage", kind::full, false);
    run("frame, cursor 64x64 moving", kind::cursor, false);
    run("frame, one terminal line", kind::line, false);
    comp.shutdown();
}

/// Stacked windows over a scaled opaque wallpaper. A client-side-decorated one (`csd`) is
/// drawn as GTK 4 / libadwaita draws it: an ARGB buffer with a 24 px shadow margin and 12 px
/// rounded corners, opaque only inside the opaque region it sends (three bands,
/// `csd_bands`). Any other window is an opaque buffer of its content size (server-side
/// decorations, a panel). The scene is built from surface nodes and published by
/// `commit_frame` (emission, the visibility pass, batching); every backend then composites
/// the published draws. Prints draws culled, pixels drawn and pixels blended (the area each
/// backend touches: clipped draw area, culled draws excluded), the UI cost of building the
/// list plus the visibility pass and of the pass alone, and each backend's composite.
struct csd_window {
    lx::rect2i content{};
    std::uint32_t color = 0;
    bool csd = true;
};
constexpr int k_csd_shadow = 24;
constexpr int k_csd_radius = 12;

/// The opaque region GTK sends for a rounded window: the content box minus its four
/// corner squares, as the three bands a pixman region holds it in (surface-local).
void csd_bands(const csd_window& w, lx::rect2i out[3]) {
    const int m = k_csd_shadow;
    const int r = k_csd_radius;
    const int cw = w.content.width;
    const int ch = w.content.height;
    out[0] = {m + r, m, cw - 2 * r, r};
    out[1] = {m, m + r, cw, ch - 2 * r};
    out[2] = {m + r, m + ch - r, cw - 2 * r, r};
}

/// Output pixels a published draw touches: its visible rects when the visibility pass cut
/// it, else its dst clipped to its clip and the output.
unsigned long long drawn_area(const lx::scene::draw_command& d, int w, int h) {
    if (d.visible_count > 0) {
        unsigned long long px = 0;
        for (unsigned k = 0; k < d.visible_count; ++k)
            px += static_cast<unsigned long long>(d.visible[k].width) *
                  static_cast<unsigned>(d.visible[k].height);
        return px;
    }
    lx::rect2i r = d.dst;
    if (d.clip.width > 0 && d.clip.height > 0) {
        const int x0 = std::max(r.x, d.clip.x);
        const int y0 = std::max(r.y, d.clip.y);
        const int x1 = std::min(r.x + r.width, d.clip.x + d.clip.width);
        const int y1 = std::min(r.y + r.height, d.clip.y + d.clip.height);
        r = {x0, y0, x1 - x0, y1 - y0};
    }
    const int x0 = std::max(r.x, 0);
    const int y0 = std::max(r.y, 0);
    const int x1 = std::min(r.x + r.width, w);
    const int y1 = std::min(r.y + r.height, h);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    return static_cast<unsigned long long>(x1 - x0) * static_cast<unsigned>(y1 - y0);
}

void bench_occlusion(const char* name, std::span<const csd_window> windows, unsigned width,
                     unsigned height, int iters, bool vk_on_software) {
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const unsigned k_windows = static_cast<unsigned>(windows.size());
    constexpr unsigned k_wallpaper_id = 1;
    constexpr unsigned k_wallpaper_px = 256;

    // Premultiplied ARGB words (CPU) and RGBA bytes (GL, Vulkan) of each window's buffer:
    // a CSD one has a translucent shadow margin and corners and an opaque body.
    std::vector<std::vector<std::uint32_t>> argb(k_windows);
    std::vector<std::vector<unsigned char>> rgba(k_windows);
    std::vector<unsigned> tex_w(k_windows);
    std::vector<unsigned> tex_h(k_windows);
    for (unsigned i = 0; i < k_windows; ++i) {
        const csd_window& win = windows[i];
        const int margin = win.csd ? k_csd_shadow : 0;
        tex_w[i] = static_cast<unsigned>(win.content.width + 2 * margin);
        tex_h[i] = static_cast<unsigned>(win.content.height + 2 * margin);
        argb[i].assign(static_cast<std::size_t>(tex_w[i]) * tex_h[i],
                       win.csd ? 0x20000000u : win.color);
        if (win.csd) {
            lx::rect2i bands[3]{};
            csd_bands(win, bands);
            for (const lx::rect2i& b : bands)
                for (int y = b.y; y < b.y + b.height; ++y)
                    for (int x = b.x; x < b.x + b.width; ++x)
                        argb[i][static_cast<std::size_t>(y) * tex_w[i] +
                                static_cast<unsigned>(x)] = win.color;
        }
        rgba[i].resize(argb[i].size() * 4);
        for (std::size_t p = 0; p < argb[i].size(); ++p) {
            const std::uint32_t v = argb[i][p];
            rgba[i][p * 4 + 0] = static_cast<unsigned char>(v >> 16);
            rgba[i][p * 4 + 1] = static_cast<unsigned char>(v >> 8);
            rgba[i][p * 4 + 2] = static_cast<unsigned char>(v);
            rgba[i][p * 4 + 3] = static_cast<unsigned char>(v >> 24);
        }
    }
    std::vector<std::uint32_t> wall_argb(k_wallpaper_px * k_wallpaper_px, 0xFF406080u);
    std::vector<unsigned char> wall_rgba(wall_argb.size() * 4, 0xFF);
    for (std::size_t p = 0; p < wall_argb.size(); ++p) {
        wall_rgba[p * 4 + 0] = 0x40;
        wall_rgba[p * 4 + 1] = 0x60;
        wall_rgba[p * 4 + 2] = 0x80;
    }

    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    scene.set_projection({{0, 0}, 1.0, {w, h}});
    lx::scene::surface_node wallpaper{lx::surface_id{1}};
    {
        lx::gfx::imported_image img{};
        img.image_id = k_wallpaper_id;
        img.width = k_wallpaper_px;
        img.height = k_wallpaper_px;
        wallpaper.attach(img);
        wallpaper.set_bounds({0, 0, w, h});
        wallpaper.set_opaque(true);
    }
    std::vector<std::unique_ptr<lx::scene::surface_node>> nodes;
    // --animate: the top window inside its own tree container (an anchor), mid-animation.
    lx::scene::container_node animated{};
    animated.set_anchor(true);
    (void)scene.root().add(&wallpaper);
    for (unsigned i = 0; i < k_windows; ++i) {
        auto node = std::make_unique<lx::scene::surface_node>(lx::surface_id{10 + i});
        lx::gfx::imported_image img{};
        img.image_id = 10 + i;
        img.width = tex_w[i];
        img.height = tex_h[i];
        node->attach(img);
        node->set_bounds({0, 0, static_cast<int>(tex_w[i]), static_cast<int>(tex_h[i])});
        const int margin = windows[i].csd ? k_csd_shadow : 0;
        node->set_transform({windows[i].content.x - margin, windows[i].content.y - margin});
        if (windows[i].csd) {
            // What the compositor derives from the region: its two largest rects, largest
            // first.
            lx::rect2i bands[3]{};
            csd_bands(windows[i], bands);
            const lx::rect2i kept[2] = {bands[1], bands[0]};
            node->set_opaque_rects(kept);
        } else {
            node->set_opaque(true);
        }
        if (g_animate && i + 1 == k_windows) {
            const lx::rect2i c = windows[i].content;
            animated.set_transform(node->origin());
            lx::scene::node_animation a{};
            a.active = true;
            a.from_x = c.x;
            a.from_y = c.y;
            a.scale_x = a.scale_y = 0.95;
            a.to_x = c.x + c.width * 0.025;
            a.to_y = c.y + c.height * 0.025;
            a.opacity = 0.6f;
            animated.set_animation(a);
            (void)animated.add(node.get());
            (void)scene.root().add(&animated);
        } else {
            (void)scene.root().add(node.get());
        }
        nodes.push_back(std::move(node));
    }

    // UI cost: the list build plus the visibility pass, and the pass alone.
    const lx::rect2i output{0, 0, w, h};
    lx::scene::draw_list list{};
    constexpr int k_ui_reps = 20000;
    scene.build_draw_list_into(list, 0);
    (void)lx::scene::mark_occlusion(list, output, nullptr);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < k_ui_reps; ++i) {
        scene.build_draw_list_into(list, 0);
        sink += lx::scene::mark_occlusion(list, output, nullptr);
    }
    const double build_us = ms_since(t0) * 1e3 / k_ui_reps;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < k_ui_reps; ++i)
        sink += lx::scene::mark_occlusion(list, output, nullptr);
    const double mark_us = ms_since(t0) * 1e3 / k_ui_reps;

    // The published frame, as the render thread sees it.
    if (!scene.commit_frame(1))
        return;
    const unsigned culled = scene.last_culled();
    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = scene.acquire_render_snapshot();
    std::vector<lx::gfx::blit_command> blits;
    unsigned long long drawn_px = 0;
    unsigned long long blended_px = 0;
    for (unsigned i = 0; i < snap.draws().size(); ++i) {
        const lx::scene::draw_command& d = snap.draws().data()[i];
        lx::gfx::blit_command b{};
        b.texture_id = d.texture.id();
        b.dst = d.dst;
        b.src = d.src;
        b.clip = d.clip;
        b.tint = d.tint;
        b.opacity = d.opacity;
        b.blend = d.blend;
        b.buffer_xform = d.buffer_xform;
        b.scaled = d.scaled;
        b.occluded = d.occluded;
        b.visible_count = d.visible_count;
        for (unsigned k = 0; k < d.visible_count; ++k)
            b.visible[k] = d.visible[k];
        blits.push_back(b);
        if (d.occluded)
            continue;
        const unsigned long long px = drawn_area(d, w, h);
        drawn_px += px;
        if (d.blend != lx::blend_mode::opaque)
            blended_px += px;
    }
    scene.release_render_frame(snap);
    const unsigned n = static_cast<unsigned>(blits.size());
    std::printf("  -- %s (%u draws) --\n", name, n);
    std::printf("  draws culled %u, pixels drawn %llu (%.2f x output), blended %llu\n", culled,
                drawn_px, static_cast<double>(drawn_px) / (static_cast<double>(w) * h),
                blended_px);
    std::printf("  UI: build + visibility pass %7.3f us, visibility pass %7.3f us\n", build_us,
                mark_us);

    // CPU backend, one thread.
    {
        lx::gfx::cpu_compositor comp;
        comp.set_worker_threads(0);
        bool ok = comp.register_texture(k_wallpaper_id,
                                        surface_of(wall_argb, k_wallpaper_px, k_wallpaper_px,
                                                   static_cast<lx::fourcc>(
                                                       lx::pixel_format::argb8888)))
                      .has_value();
        for (unsigned i = 0; i < k_windows && ok; ++i)
            ok = comp.register_texture(10 + i, surface_of(argb[i], tex_w[i], tex_h[i],
                                                          static_cast<lx::fourcc>(
                                                              lx::pixel_format::argb8888)))
                     .has_value();
        std::vector<std::uint32_t> dst(static_cast<std::size_t>(width) * height, 0u);
        auto dst_surface = surface_of(dst, width, height, k_xrgb);
        if (ok && comp.composite(dst_surface, lx::color::rgb(0.f, 0.f, 0.f), blits.data(), n, {})) {
            t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < iters; ++i)
                (void)comp.composite(dst_surface, lx::color::rgb(0.f, 0.f, 0.f), blits.data(), n,
                                     {});
            std::printf("  CPU composite, 1 thread                    %7.3f ms\n",
                        ms_since(t0) / iters);
            if (g_hash)
                std::printf("  hash cpu %016llx\n",
                            static_cast<unsigned long long>(fnv1a(
                                reinterpret_cast<const unsigned char*>(dst.data()), dst.size() * 4)));
        }
    }

    // GL backend on a hardware renderer, the frame's fence waited (GPU time included).
    if (const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC); fd >= 0) {
        auto device = lx::gfx::egl_device::create(fd);
        if (device && !device.value().is_software_renderer()) {
            auto& dev = device.value();
            auto target = lx::gfx::gl_scanout_target::create(dev, width, height, k_xrgb);
            lx::gfx::gl_compositor comp;
            bool ok = target && comp.initialize(dev) &&
                      comp.upload_rgba(k_wallpaper_id, k_wallpaper_px, k_wallpaper_px,
                                       wall_rgba.data());
            for (unsigned i = 0; i < k_windows && ok; ++i)
                ok = comp.upload_rgba(10 + i, tex_w[i], tex_h[i], rgba[i].data()).has_value();
            const auto draw = [&] {
                auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f),
                                            blits.data(), n, {});
                if (stats && stats.value().out_fence_fd >= 0) {
                    pollfd pfd{stats.value().out_fence_fd, POLLIN, 0};
                    (void)::poll(&pfd, 1, 1000);
                    ::close(stats.value().out_fence_fd);
                }
            };
            if (ok) {
                draw();
                draw();
                t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < iters; ++i)
                    draw();
                std::printf("  GL composite, fence waited                 %7.3f ms\n",
                            ms_since(t0) / iters);
                if (g_hash) {
                    std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
                    if (comp.read_back(target.value(), px.data(),
                                       static_cast<unsigned>(px.size())))
                        std::printf("  hash gl  %016llx\n",
                                    static_cast<unsigned long long>(fnv1a(px.data(), px.size())));
                }
            }
            comp.shutdown();
        }
        ::close(fd);
    }

    // Vulkan (lavapipe only with --vk), the frame's fence waited.
    if (auto selected = lx::gfx::device_selector::select_best()) {
        auto device = std::move(selected).value();
        lx::gfx::vulkan_compositor comp;
        if ((!device.info().is_software_rasterizer || vk_on_software) &&
            comp.initialize(device.context())) {
            comp.set_gpu_timing(g_gpu_timing);
            gpu_times gpu{};
            bool ok = comp.upload_rgba(k_wallpaper_id, k_wallpaper_px, k_wallpaper_px,
                                       wall_rgba.data())
                          .has_value();
            for (unsigned i = 0; i < k_windows && ok; ++i)
                ok = comp.upload_rgba(10 + i, tex_w[i], tex_h[i], rgba[i].data()).has_value();
            auto target = lx::gfx::render_target::create(device.context(), width, height);
            if (ok && target) {
                const auto draw = [&] {
                    auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f),
                                                blits.data(), n);
                    if (stats && stats.value().out_fence_fd >= 0) {
                        pollfd pfd{stats.value().out_fence_fd, POLLIN, 0};
                        (void)::poll(&pfd, 1, 1000);
                        ::close(stats.value().out_fence_fd);
                    }
                    if (g_gpu_timing)
                        gpu.take(comp);
                };
                draw();
                gpu = {};
                rusage r0{};
                ::getrusage(RUSAGE_SELF, &r0);
                t0 = std::chrono::steady_clock::now();
                const int vk_iters = iters / 4 > 0 ? iters / 4 : 1;
                for (int i = 0; i < vk_iters; ++i)
                    draw();
                const double wall = ms_since(t0) / vk_iters;
                rusage r1{};
                ::getrusage(RUSAGE_SELF, &r1);
                const auto cpu_us = [](const rusage& r) {
                    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e6 + r.ru_utime.tv_usec +
                           r.ru_stime.tv_usec;
                };
                if (g_gpu_timing)
                    gpu.take(comp);
                std::printf("  Vulkan composite, fence waited             %7.3f ms  (cpu %.1f ms)",
                            wall, (cpu_us(r1) - cpu_us(r0)) / 1e3 / vk_iters);
                gpu.print();
                if (g_hash) {
                    std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
                    if (comp.read_back(target.value(), px.data(),
                                       static_cast<unsigned>(px.size())))
                        std::printf("  hash vk  %016llx\n",
                                    static_cast<unsigned long long>(fnv1a(px.data(), px.size())));
                }
            }
            comp.shutdown();
        }
    }
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    for (auto& node : nodes) {
        (void)scene.root().remove(node.get());
        (void)animated.remove(node.get());
    }
    (void)scene.root().remove(&animated);
    (void)scene.root().remove(&wallpaper);
}

/// A whole-output shm upload on Vulkan (`vulkan_compositor::upload_rgba`), both ways: the
/// host writing the image (VK_EXT_host_image_copy) and the staging ring plus a queue copy.
/// Each upload waits for the frames in flight first (as the compositor's does), so the time
/// is the render thread's; the last upload's GPU copy is waited for at the end.
void bench_vk_upload(unsigned width, unsigned height, int iters, bool on_software) {
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected)
        return;
    auto device = std::move(selected).value();
    if (!device.context().valid() || (device.info().is_software_rasterizer && !on_software))
        return;
    const lx::gfx::vk_context ctx = device.context();
    lx::gfx::host_image_copier probe{};
    const bool host = probe.init(ctx, 37u /* VK_FORMAT_R8G8B8A8_UNORM */);
    std::printf("  -- Vulkan shm upload (%ux%u, %s) --\n", width, height,
                !device.info().supports_host_image_copy
                    ? "no VK_EXT_host_image_copy"
                    : host ? "host image copy usable"
                           : "host image copy not usable for RGBA8 (format, layout or "
                             "optimalDeviceAccess)");
    std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4u, 0x60);
    const auto run = [&](const char* label, lx::gfx::vk_context c) {
        lx::gfx::vulkan_compositor comp;
        if (!comp.initialize(c))
            return;
        static const unsigned char k_one[4] = {0, 0, 0, 255};
        if (!comp.upload_rgba(1, width, height, px.data()))
            return; // the image made, outside the timing
        rusage r0{};
        ::getrusage(RUSAGE_SELF, &r0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            px[static_cast<std::size_t>(i) * 4u % px.size()] = static_cast<unsigned char>(i);
            if (!comp.upload_rgba(1, width, height, px.data()))
                return;
        }
        (void)comp.upload_rgba(2, 1, 1, k_one); // waits for the last copy
        const double wall = ms_since(t0) / iters;
        rusage r1{};
        ::getrusage(RUSAGE_SELF, &r1);
        const auto cpu_us = [](const rusage& r) {
            return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e6 + r.ru_utime.tv_usec +
                   r.ru_stime.tv_usec;
        };
        std::printf("  %-42s %7.3f ms  (cpu %.2f ms)\n", label, wall,
                    (cpu_us(r1) - cpu_us(r0)) / 1e3 / iters);
        comp.shutdown();
    };
    if (host)
        run("upload, host image copy", ctx);
    lx::gfx::vk_context staged = ctx;
    staged.supports_host_image_copy = false;
    run("upload, staging ring + queue copy", staged);
}

} // namespace

/// --blur: what a panel-sized backdrop blur (ext-background-effect-v1) costs per frame — a
/// 32 px panel blurring a full-screen window, fence waited, on GL and (with --vk) Vulkan. Each
/// case against the same frame without the blur draw: a full repaint and a panel-only update
/// with the cached blur, and with the backdrop changing every frame (blurred again, the
/// frame grown to the footprint); and one text row of the window updated far from the
/// panel while the window's commits leave the backdrop stale (the row stays its own rect,
/// the footprint is redrawn beside it). Never runs without the flag, so the default output
/// stays what it was.
bool g_blur = false;

template <typename Comp, typename Composite>
void blur_cases(const char* backend, Comp& comp, Composite composite, unsigned width,
                unsigned height, int iters) {
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const lx::rect2i panel{0, 0, w, 32};
    lx::gfx::blit_command window{};
    window.texture_id = 1;
    window.dst = {0, 0, w, h};
    window.blend = lx::blend_mode::opaque;
    lx::gfx::blit_command blur{};
    blur.blur = true;
    blur.dst = panel;
    blur.blur_box = panel;
    blur.blend = lx::blend_mode::premultiplied;
    lx::gfx::blit_command overlay{};
    overlay.solid = true;
    overlay.solid_color = lx::color::rgb(0.08f, 0.08f, 0.09f, 0.72f);
    overlay.dst = panel;
    overlay.blend = lx::blend_mode::premultiplied;
    const lx::gfx::blit_command with[3] = {window, blur, overlay};
    const lx::gfx::blit_command without[2] = {window, overlay};
    lx::gfx::blur_source src{panel, true, 1};
    enum class damage { clock, full, row };
    const auto run = [&](const char* label, bool blurred, bool changing, damage where) {
        unsigned long long area = 0;
        const auto frame = [&](int i) {
            lx::gfx::damage_region d{};
            if (where == damage::full)
                d.set_full();
            else if (where == damage::clock)
                d.add({w - 200, 4, 120, 24}); // the panel's clock
            else
                d.add({w / 4, h / 2, w / 2, 16}); // one text row of the window, far below
            if (changing) {
                // The window under the panel committed: its content serial is in the
                // backdrop's signature. Beside the row, it moves under the footprint.
                src.backdrop = 1000u + static_cast<unsigned>(i);
                if (where != damage::row)
                    d.add({100 + (i % 2) * 8, 40, 64, 40});
            }
            if (blurred)
                (void)comp.begin_blur_frame(0, src, d, w, h);
            composite(blurred ? with : without, blurred ? 3u : 2u, d);
            if (blurred)
                comp.end_blur_frame();
            area = d.full ? static_cast<unsigned long long>(w) * h : d.area();
        };
        frame(0);
        frame(1);
        const unsigned long long runs0 = comp.blur_runs();
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i)
            frame(2 + i);
        std::printf("  %s %-44s %7.3f ms  (blurred %llu of %d, %llu px)\n", backend, label,
                    ms_since(t0) / iters, comp.blur_runs() - runs0, iters, area);
    };
    run("full repaint, no blur draw", false, false, damage::full);
    run("full repaint, cached blur", true, false, damage::full);
    run("full repaint, backdrop changing (blurred again)", true, true, damage::full);
    run("panel update, no blur draw", false, false, damage::clock);
    run("panel update, cached blur", true, false, damage::clock);
    run("panel update + backdrop changing (blurred again)", true, true, damage::clock);
    run("backdrop changing, no blur draw", false, true, damage::clock);
    run("window row update, no blur draw", false, true, damage::row);
    run("window row update, backdrop stale (blurred again)", true, true, damage::row);
    comp.release_blur(0);
}

void bench_blur(unsigned width, unsigned height, int iters, bool vk_on_software) {
    const lx::rect2i f = lx::gfx::blur_footprint({0, 0, static_cast<int>(width), 32},
                                                 static_cast<int>(width), static_cast<int>(height));
    unsigned long long texels = 0;
    for (unsigned k = 0; k < lx::gfx::k_blur_levels; ++k) {
        const lx::size2i l = lx::gfx::blur_level_size(f, k);
        texels += static_cast<unsigned long long>(l.width) * l.height;
    }
    std::printf("  -- backdrop blur, a %ux32 panel: footprint %dx%d, levels %llu texels "
                "(GL %.2f MiB, Vulkan %.2f MiB) --\n",
                width, f.width, f.height, texels, texels * 4 / 1048576.0, texels * 8 / 1048576.0);
    std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            unsigned char* p = &px[(static_cast<std::size_t>(y) * width + x) * 4];
            p[0] = static_cast<unsigned char>(x * 3);
            p[1] = static_cast<unsigned char>(y * 5);
            p[2] = ((x / 16 + y / 16) % 2) ? 220 : 40;
            p[3] = 255;
        }
    const auto wait = [](int fd) {
        if (fd >= 0) {
            pollfd pfd{fd, POLLIN, 0};
            (void)::poll(&pfd, 1, 1000);
            ::close(fd);
        }
    };
    if (const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC); fd >= 0) {
        auto device = lx::gfx::egl_device::create(fd);
        if (device && !device.value().is_software_renderer()) {
            auto& dev = device.value();
            auto target = lx::gfx::gl_scanout_target::create(dev, width, height, k_xrgb);
            lx::gfx::gl_compositor comp;
            if (target && comp.initialize(dev) && comp.supports_blur() &&
                comp.upload_rgba(1, width, height, px.data())) {
                struct gl_comp {
                    lx::gfx::gl_compositor& c;
                    unsigned fb;
                    bool begin_blur_frame(unsigned o, const lx::gfx::blur_source& s,
                                          lx::gfx::damage_region& d, int w, int h) {
                        return c.begin_blur_frame(o, fb, {&s, 1}, d, w, h);
                    }
                    void end_blur_frame() { c.end_blur_frame(); }
                    void release_blur(unsigned o) { c.release_blur(o); }
                    unsigned long long blur_runs() const { return c.blur_runs(); }
                } g{comp, target.value().framebuffer()};
                blur_cases("GL    ", g,
                           [&](const lx::gfx::blit_command* cmds, unsigned n,
                               const lx::gfx::damage_region& d) {
                               auto st = comp.composite_region(target.value(),
                                                               lx::color::rgb(0.f, 0.f, 0.f),
                                                               cmds, n, d);
                               if (st)
                                   wait(st.value().out_fence_fd);
                           },
                           width, height, iters);
            }
            comp.shutdown();
        }
        ::close(fd);
    }
    if (auto selected = lx::gfx::device_selector::select_best()) {
        auto device = std::move(selected).value();
        lx::gfx::vulkan_compositor comp;
        if ((!device.info().is_software_rasterizer || vk_on_software) &&
            comp.initialize(device.context()) && comp.supports_blur() &&
            comp.upload_rgba(1, width, height, px.data())) {
            auto target = lx::gfx::render_target::create(device.context(), width, height);
            if (target) {
                struct vk_comp {
                    lx::gfx::vulkan_compositor& c;
                    bool begin_blur_frame(unsigned o, const lx::gfx::blur_source& s,
                                          lx::gfx::damage_region& d, int w, int h) {
                        return c.begin_blur_frame(o, {&s, 1}, d, w, h);
                    }
                    void end_blur_frame() { c.end_blur_frame(); }
                    void release_blur(unsigned o) { c.release_blur(o); }
                    unsigned long long blur_runs() const { return c.blur_runs(); }
                } v{comp};
                blur_cases("Vulkan", v,
                           [&](const lx::gfx::blit_command* cmds, unsigned n,
                               const lx::gfx::damage_region& d) {
                               auto st = comp.composite_region(target.value(),
                                                               lx::color::rgb(0.f, 0.f, 0.f),
                                                               cmds, n, d, d, &target.value());
                               if (st)
                                   wait(st.value().out_fence_fd);
                           },
                           width, height, iters / 4 > 0 ? iters / 4 : 1);
            }
        }
        comp.shutdown();
    }
}

/// `--color`: what a colour-managed surface (wp_color_manager_v1) costs. A full-screen
/// window repainted whole, undescribed and then described three ways — gamma 2.2 sRGB (a
/// decode curve, identity matrix), the sRGB curve on BT.2020 (a matrix) and PQ BT.2020 at
/// its defaults (curve, matrix and tone mapping) — on each backend: CPU (1 and 4 threads),
/// GL (fence waited) and Vulkan (fence waited; `--vk` for a software device). Never runs
/// without the flag, so the default output stays what it was.
bool g_downsample = false;

void bench_downsample(unsigned width, unsigned height, int iters, bool vk_on_software) {
    std::printf("  -- downsampled fullscreen surface (render scale over the output scale) --\n");
    struct ratio {
        const char* label;
        unsigned num, den;
    };
    const ratio ratios[] = {{"1:1", 1, 1}, {"2:1 (render 2, scale 1)", 2, 1},
                            {"4:3 (render 2, scale 1.5)", 4, 3},
                            {"8:5 (render 2, scale 1.25)", 8, 5}};
    const auto wait = [](int fd) {
        if (fd >= 0) {
            pollfd pfd{fd, POLLIN, 0};
            (void)::poll(&pfd, 1, 1000);
            ::close(fd);
        }
    };
    const int gl_fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    std::optional<lx::gfx::egl_device> gl_device;
    if (gl_fd >= 0)
        if (auto d = lx::gfx::egl_device::create(gl_fd); d && !d.value().is_software_renderer())
            gl_device.emplace(std::move(d).value());
    const bool gl_ok = gl_device.has_value();
    lx::gfx::device vk_device{};
    bool vk_ok = false;
    if (auto selected = lx::gfx::device_selector::select_best()) {
        vk_device = std::move(selected).value();
        vk_ok = !vk_device.info().is_software_rasterizer || vk_on_software;
    }
    for (const ratio& r : ratios) {
        const unsigned sw = width * r.num / r.den, sh = height * r.num / r.den;
        std::vector<std::uint32_t> words(static_cast<std::size_t>(sw) * sh);
        std::vector<unsigned char> rgba(words.size() * 4);
        for (unsigned y = 0; y < sh; ++y)
            for (unsigned x = 0; x < sw; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * sw + x;
                const unsigned v = ((x % 7) == 0 || (y % 5) == 0) ? 20u : 235u; // text-like strokes
                words[i] = 0xFF000000u | (v * 0x010101u);
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = static_cast<unsigned char>(v);
                rgba[i * 4 + 3] = 255;
            }
        lx::gfx::blit_command cmd{};
        cmd.texture_id = 1;
        cmd.dst = {0, 0, static_cast<int>(width), static_cast<int>(height)};
        cmd.blend = lx::blend_mode::opaque;
        const auto timed = [&](const char* backend, auto&& frame) {
            frame(); // warm: programs, pipelines, page faults
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < iters; ++i)
                frame();
            std::printf("  %s %-28s %8.3f ms\n", backend, r.label, ms_since(t0) / iters);
        };
        {
            std::vector<std::uint32_t> dst(static_cast<std::size_t>(width) * height, 0u);
            lx::gfx::cpu_compositor comp;
            if (comp.register_texture(1, surface_of(words, sw, sh, k_xrgb))) {
                const auto out = surface_of(dst, width, height, k_xrgb);
                timed("CPU 1t ", [&] {
                    (void)comp.composite(out, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
                });
            }
        }
        if (gl_ok) {
            auto& dev = *gl_device;
            auto target = lx::gfx::gl_scanout_target::create(dev, width, height, k_xrgb);
            lx::gfx::gl_compositor comp;
            if (target && comp.initialize(dev) && comp.upload_rgba(1, sw, sh, rgba.data()))
                timed("GL     ", [&] {
                    auto st = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd,
                                             1, {});
                    if (st)
                        wait(st.value().out_fence_fd);
                });
            comp.shutdown();
        }
        if (vk_ok) {
            lx::gfx::vulkan_compositor comp;
            if (comp.initialize(vk_device.context()) && comp.upload_rgba(1, sw, sh, rgba.data())) {
                if (auto target = lx::gfx::render_target::create(vk_device.context(), width, height))
                    timed("Vulkan ", [&] {
                        auto st = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f),
                                                 &cmd, 1);
                        if (st)
                            wait(st.value().out_fence_fd);
                    });
            }
            comp.shutdown();
        }
    }
    if (gl_fd >= 0)
        ::close(gl_fd);
}

bool g_color = false;

void bench_color(unsigned width, unsigned height, int iters, bool vk_on_software) {
    std::printf("  -- colour-managed surface (wp_color_manager_v1), full repaint --\n");
    std::vector<std::uint32_t> words(static_cast<std::size_t>(width) * height);
    std::vector<unsigned char> rgba(words.size() * 4);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            const unsigned r = (x * 3) & 0xFF, g = (y * 5) & 0xFF, b = ((x / 16 + y / 16) % 2) ? 220 : 40;
            words[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
            rgba[i * 4] = static_cast<unsigned char>(r);
            rgba[i * 4 + 1] = static_cast<unsigned char>(g);
            rgba[i * 4 + 2] = static_cast<unsigned char>(b);
            rgba[i * 4 + 3] = 255;
        }
    struct variant {
        const char* label;
        lx::gfx::color_transform t;
    };
    variant variants[4] = {{"undescribed (sRGB)", {}}, {"gamma 2.2 sRGB", {}},
                           {"sRGB curve BT.2020", {}}, {"PQ BT.2020", {}}};
    {
        lx::gfx::color_description g22{};
        g22.tf = lx::gfx::color_tf::power;
        lx::gfx::color_description wide{};
        wide.primaries = lx::gfx::primaries_of(lx::gfx::named_primaries::bt2020);
        lx::gfx::color_description pq = wide;
        pq.tf = lx::gfx::color_tf::st2084_pq;
        lx::gfx::set_default_luminances(pq);
        (void)lx::gfx::make_color_transform(g22, variants[1].t);
        (void)lx::gfx::make_color_transform(wide, variants[2].t);
        (void)lx::gfx::make_color_transform(pq, variants[3].t);
    }
    const auto draw = [&](const variant& v) {
        lx::gfx::blit_command cmd{};
        cmd.texture_id = 1;
        cmd.dst = {0, 0, static_cast<int>(width), static_cast<int>(height)};
        cmd.blend = lx::blend_mode::opaque;
        cmd.color = v.t;
        return cmd;
    };
    const auto timed = [&](const char* backend, const variant& v, auto&& frame) {
        const lx::gfx::blit_command cmd = draw(v);
        frame(cmd); // warm: pipelines, programs, page faults
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i)
            frame(cmd);
        std::printf("  %s %-24s %8.3f ms\n", backend, v.label, ms_since(t0) / iters);
    };
    for (const unsigned workers : {0u, 3u}) {
        std::vector<std::uint32_t> dst(words.size(), 0u);
        lx::gfx::cpu_compositor comp;
        comp.set_worker_threads(workers);
        if (!comp.register_texture(1, surface_of(words, width, height, k_xrgb)))
            break;
        const auto out = surface_of(dst, width, height, k_xrgb);
        for (const variant& v : variants)
            timed(workers ? "CPU 4t " : "CPU 1t ", v, [&](const lx::gfx::blit_command& c) {
                (void)comp.composite(out, lx::color::rgb(0.f, 0.f, 0.f), &c, 1, {});
            });
    }
    const auto wait = [](int fd) {
        if (fd >= 0) {
            pollfd pfd{fd, POLLIN, 0};
            (void)::poll(&pfd, 1, 1000);
            ::close(fd);
        }
    };
    if (const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC); fd >= 0) {
        auto device = lx::gfx::egl_device::create(fd);
        if (device && !device.value().is_software_renderer()) {
            auto& dev = device.value();
            auto target = lx::gfx::gl_scanout_target::create(dev, width, height, k_xrgb);
            lx::gfx::gl_compositor comp;
            if (target && comp.initialize(dev) && comp.supports_color_management() &&
                comp.upload_rgba(1, width, height, rgba.data())) {
                for (const variant& v : variants)
                    timed("GL     ", v, [&](const lx::gfx::blit_command& c) {
                        auto st = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &c,
                                                 1, {});
                        if (st)
                            wait(st.value().out_fence_fd);
                    });
            }
            comp.shutdown();
        }
        ::close(fd);
    }
    if (auto selected = lx::gfx::device_selector::select_best()) {
        auto device = std::move(selected).value();
        lx::gfx::vulkan_compositor comp;
        if ((!device.info().is_software_rasterizer || vk_on_software) &&
            comp.initialize(device.context()) && comp.supports_color_management() &&
            comp.upload_rgba(1, width, height, rgba.data())) {
            auto target = lx::gfx::render_target::create(device.context(), width, height);
            if (target) {
                for (const variant& v : variants)
                    timed("Vulkan ", v, [&](const lx::gfx::blit_command& c) {
                        auto st = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f),
                                                 &c, 1);
                        if (st)
                            wait(st.value().out_fence_fd);
                    });
            }
        }
        comp.shutdown();
    }
}

int main(int argc, char** argv) {
    unsigned width = 1918;
    unsigned height = 928;
    bool vk_on_software = false;
    for (bool more = true; more && argc >= 2;) {
        more = false;
        if (std::strcmp(argv[argc - 1], "--vk") == 0) {
            vk_on_software = true;
            --argc;
            more = true;
        } else if (std::strcmp(argv[argc - 1], "--gpu-timing") == 0) {
            g_gpu_timing = true;
            --argc;
            more = true;
        } else if (std::strcmp(argv[argc - 1], "--hash") == 0) {
            g_hash = true;
            --argc;
            more = true;
        } else if (std::strcmp(argv[argc - 1], "--animate") == 0) {
            g_animate = true;
            --argc;
            more = true;
        } else if (std::strcmp(argv[argc - 1], "--blur") == 0) {
            g_blur = true;
            --argc;
            more = true;
        } else if (std::strcmp(argv[argc - 1], "--color") == 0) {
            g_color = true;
            --argc;
            more = true;
        } else if (std::strcmp(argv[argc - 1], "--downsample") == 0) {
            g_downsample = true;
            --argc;
            more = true;
        }
    }
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

    if (g_downsample) {
        // Alone, as --blur.
        bench_downsample(width, height, k_iters, vk_on_software);
        return 0;
    }
    if (g_color) {
        // Alone, as --blur.
        bench_color(width, height, k_iters, vk_on_software);
        return 0;
    }
    if (g_blur) {
        // Alone: the blur's cost is what this run is for.
        bench_blur(width, height, k_iters, vk_on_software);
        return 0;
    }
    bench_gl(width, height, k_iters);
    bench_vk(width, height, k_iters / 2, vk_on_software);
    bench_vk_upload(width, height, k_iters, vk_on_software);
    bench_vk_damage(width, height, k_iters, vk_on_software);
    {
        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);
        // Bottom to top. The editor is on top; terminal and notes lie under its body, files
        // and chat peek out at its left/bottom and right edges.
        const csd_window stacked[] = {
            {{w * 3 / 100, h * 35 / 100, w * 45 / 100, h * 55 / 100}, 0xFF305070u}, // files
            {{w * 70 / 100, h * 12 / 100, w * 27 / 100, h * 70 / 100}, 0xFF507030u}, // chat
            {{w * 25 / 100, h * 25 / 100, w * 40 / 100, h * 45 / 100}, 0xFF202020u}, // terminal
            {{w * 50 / 100, h * 45 / 100, w * 20 / 100, h * 30 / 100}, 0xFFE0D070u}, // notes
            {{w * 15 / 100, h * 8 / 100, w * 70 / 100, h * 80 / 100}, 0xFFF0F0F0u},  // editor
        };
        bench_occlusion("stacked CSD windows", stacked, width, height, k_iters, vk_on_software);
        // Cascaded windows, each 47% x 56% of the output, 8% / 9% apart.
        csd_window cascade[6]{};
        for (int i = 0; i < 6; ++i)
            cascade[i] = {{w * (5 + 8 * i) / 100, h * (7 + 9 * i) / 100, w * 47 / 100,
                           h * 56 / 100},
                          0xFF203040u + static_cast<std::uint32_t>(i) * 0x00181818u, false};
        bench_occlusion("3 cascaded opaque windows", std::span{cascade, 3}, width, height,
                        k_iters / 4, vk_on_software);
        bench_occlusion("6 cascaded opaque windows", std::span{cascade, 6}, width, height,
                        k_iters / 4, vk_on_software);
        for (csd_window& c : cascade)
            c.csd = true;
        bench_occlusion("4 cascaded CSD windows", std::span{cascade, 4}, width, height,
                        k_iters / 4, vk_on_software);
        // A maximised window under a 32 px panel, over three others; and two tiled halves.
        csd_window maximised[5] = {cascade[0], cascade[1], cascade[2],
                                   {{0, 32, w, h - 32}, 0xFFF0F0F0u, false},
                                   {{0, 0, w, 32}, 0xFF101010u, false}};
        for (unsigned i = 0; i < 3; ++i)
            maximised[i].csd = false;
        bench_occlusion("maximised window under a panel", maximised, width, height, k_iters / 4,
                        vk_on_software);
        const csd_window tiled[2] = {{{0, 0, w / 2, h}, 0xFF202020u, false},
                                     {{w / 2, 0, w - w / 2, h}, 0xFFF0F0F0u, false}};
        bench_occlusion("two tiled halves", tiled, width, height, k_iters / 4, vk_on_software);
        // A fullscreen window under a small opaque 300x400 menu: the cut would narrow 400 rows
        // by only 16 %, which costs the CPU path more than it saves, so it draws whole.
        const csd_window menu[2] = {{{0, 0, w, h}, 0xFFF0F0F0u, false},
                                    {{w * 40 / 100, h * 30 / 100, 300, 400}, 0xFF303030u, false}};
        bench_occlusion("fullscreen window + small opaque menu", menu, width, height, k_iters,
                        vk_on_software);
    }

    // Never fail on timing — CTest only checks exit status.
    return 0;
}
