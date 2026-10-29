// End-to-end check that a real client buffer's pixels reach the composited output.
//
// This exists because the previous render path could not fail this class of bug: an
// FD-less dmabuf desc produced a fabricated VkImage, and an unresolved texture produced a
// magenta placeholder, so a golden-image test passed while sampling nothing. Every
// assertion below is on pixels that could only come from the source buffer.
#include "lumen_test.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_DRM)
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

import std;
import lx.foundation;
import lx.gfx;

namespace {

constexpr unsigned k_width = 64;
constexpr unsigned k_height = 64;

/// Left half opaque red, right half opaque blue, in little-endian XRGB8888 as a Wayland
/// client would write it, with `x` in the undefined fourth byte.
void fill_pattern(unsigned char* pixels, unsigned width, unsigned height, unsigned stride,
                  unsigned char x = 255u) {
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            unsigned char* px = pixels + y * stride + x * 4u;
            const bool left = x < width / 2u;
            px[0] = left ? 0u : 255u;   // B
            px[1] = 0u;                 // G
            px[2] = left ? 255u : 0u;   // R
            px[3] = x;                  // X/A
        }
    }
}

/// Composites and takes ownership of the frame's sync_file. Present is non-blocking now,
/// so every composite hands back a fence the caller must consume or close — leaking one
/// per call would exhaust the descriptor table in a long run.
[[nodiscard]] lx::result<lx::gfx::composite_stats> composite_owned(
    lx::gfx::vulkan_compositor& comp, lx::gfx::render_target& target, lx::color clear,
    const lx::gfx::blit_command* cmds, unsigned count) {
    auto stats = comp.composite(target, clear, cmds, count);
    if (stats && stats.value().out_fence_fd >= 0)
        ::close(stats.value().out_fence_fd);
    return stats;
}

struct gpu_fixture {
    lx::gfx::device device{};
    // Declared before the compositor, destroyed after it: the compositor's destructor waits
    // for the frames it submitted, which render into the target.
    lx::gfx::render_target target{};
    lx::gfx::vulkan_compositor compositor{};
    bool usable = false;
};

/// Brings up a real Vulkan device, or reports why the test cannot run. A missing GPU is a
/// skip; a GPU that fails to composite is a failure. Filled in place because a live
/// Vulkan compositor owns handles and is deliberately not movable.
bool setup_gpu(gpu_fixture& fixture) {
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected) {
        std::printf("SKIP: no graphics device\n");
        return false;
    }
    fixture.device = std::move(selected).value();
    if (!fixture.device.info().supports_dmabuf_import) {
        std::printf("SKIP: device has no dma-buf import (headless fallback)\n");
        return false;
    }

    if (auto ready = fixture.compositor.initialize(fixture.device.context()); !ready) {
        std::printf("SKIP: composite pass unavailable: %s\n", ready.get_error().message);
        return false;
    }

    auto created = lx::gfx::render_target::create(fixture.device.context(), k_width, k_height);
    if (!created) {
        std::printf("SKIP: no render target: %s\n", created.get_error().message);
        return false;
    }
    fixture.target = std::move(created).value();
    fixture.usable = true;
    return true;
}

#if defined(LUMEN_HAS_DRM)

/// A dumb KMS buffer exported as a dma-buf: the closest stand-in for a client buffer that
/// does not require a live Wayland client.
struct dumb_dmabuf {
    lx::unique_fd card{};
    lx::unique_fd dmabuf{};
    unsigned handle = 0;
    unsigned stride = 0;
    bool valid = false;
};

dumb_dmabuf make_dumb_dmabuf(unsigned char x = 255u) {
    dumb_dmabuf out{};

    for (const char* path : {"/dev/dri/card0", "/dev/dri/card1"}) {
        const int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            out.card.reset(fd);
            break;
        }
    }
    if (out.card.get() < 0)
        return out;

    drm_mode_create_dumb create{};
    create.width = k_width;
    create.height = k_height;
    create.bpp = 32;
    if (drmIoctl(out.card.get(), DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
        return out;
    out.handle = create.handle;
    out.stride = create.pitch;

    drm_mode_map_dumb map{};
    map.handle = create.handle;
    if (drmIoctl(out.card.get(), DRM_IOCTL_MODE_MAP_DUMB, &map) != 0)
        return out;

    auto* pixels = static_cast<unsigned char*>(::mmap(nullptr, create.size, PROT_READ | PROT_WRITE,
                                                     MAP_SHARED, out.card.get(),
                                                     static_cast<off_t>(map.offset)));
    if (pixels == MAP_FAILED)
        return out;
    fill_pattern(pixels, k_width, k_height, out.stride, x);
    ::munmap(pixels, create.size);

    int export_fd = -1;
    if (drmPrimeHandleToFD(out.card.get(), create.handle, DRM_CLOEXEC | DRM_RDWR, &export_fd) != 0 ||
        export_fd < 0) {
        return out;
    }
    out.dmabuf.reset(export_fd);
    out.valid = true;
    return out;
}

#endif // LUMEN_HAS_DRM

} // namespace

// An unregistered texture must be reported, never substituted with a stand-in.
LUMEN_TEST(headless_rejects_unregistered_texture) {
    lx::gfx::headless_backend hb{};
    LUMEN_CHECK(static_cast<bool>(hb.create(4, 4)));
    LUMEN_CHECK(static_cast<bool>(hb.begin_frame()));
    LUMEN_CHECK(!hb.blit_texture(9999, {0, 0, 4, 4}));
}

LUMEN_TEST(composite_reports_unregistered_draw) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = 12345; // never registered
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 0);
    LUMEN_CHECK(stats.value().draws_skipped == 1);
}

// shm pixels must survive upload → composite → readback unchanged.
LUMEN_TEST(shm_upload_reaches_composited_output) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    // Source is RGBA here, matching what the protocol layer converts shm buffers into.
    unsigned char source[k_width * k_height * 4]{};
    for (unsigned y = 0; y < k_height; ++y) {
        for (unsigned x = 0; x < k_width; ++x) {
            unsigned char* px = source + (y * k_width + x) * 4u;
            const bool left = x < k_width / 2u;
            px[0] = left ? 255u : 0u;
            px[1] = 0u;
            px[2] = left ? 0u : 255u;
            px[3] = 255u;
        }
    }

    constexpr unsigned texture_id = 0x4000'0001u;
    auto uploaded = gpu.compositor.upload_rgba(texture_id, k_width, k_height, source);
    LUMEN_CHECK(static_cast<bool>(uploaded));

    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_skipped == 0);
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    unsigned char readback[k_width * k_height * 4]{};
    auto read = gpu.compositor.read_back(gpu.target, readback, sizeof(readback));
    LUMEN_CHECK(static_cast<bool>(read));

    // Sample inside each half, away from the seam where filtering could blend.
    const auto* left = readback + ((k_height / 2u) * k_width + k_width / 4u) * 4u;
    const auto* right = readback + ((k_height / 2u) * k_width + (3u * k_width) / 4u) * 4u;

    LUMEN_CHECK(left[0] > 200 && left[1] < 60 && left[2] < 60);
    LUMEN_CHECK(right[2] > 200 && right[1] < 60 && right[0] < 60);
}

// The two upload paths: the host writing the image (VK_EXT_host_image_copy, where the
// device has it) and the staging ring with a queue copy (the capability masked) composite
// to the same pixels — a second upload over the first included.
LUMEN_TEST(shm_upload_host_copy_matches_staging) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    lx::gfx::host_image_copier probe{};
    const bool host = probe.init(gpu.device.context(), 37u /* VK_FORMAT_R8G8B8A8_UNORM */);
    std::printf("    host image copy %s\n", host ? "usable" : "not usable here: both runs stage");

    std::vector<unsigned char> first(k_width * k_height * 4u);
    std::vector<unsigned char> second(k_width * k_height * 4u);
    for (unsigned i = 0; i < k_width * k_height; ++i) {
        const unsigned x = i % k_width;
        const unsigned y = i / k_width;
        first[i * 4u + 0] = static_cast<unsigned char>(x * 4u);
        first[i * 4u + 1] = static_cast<unsigned char>(y * 4u);
        first[i * 4u + 2] = 0x80u;
        first[i * 4u + 3] = 255u;
        second[i * 4u + 0] = static_cast<unsigned char>(255u - y * 4u);
        second[i * 4u + 1] = static_cast<unsigned char>(x * 3u);
        second[i * 4u + 2] = static_cast<unsigned char>((x ^ y) * 4u);
        second[i * 4u + 3] = 255u;
    }
    const auto run = [&](lx::gfx::vk_context ctx, std::vector<unsigned char>& out) {
        lx::gfx::vulkan_compositor comp;
        if (!comp.initialize(ctx))
            return false;
        auto target = lx::gfx::render_target::create(ctx, k_width, k_height);
        if (!target)
            return false;
        constexpr unsigned id = 0x4000'0002u;
        lx::gfx::blit_command cmd{};
        cmd.texture_id = id;
        cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
        cmd.opacity = 1.f;
        cmd.blend = lx::blend_mode::opaque;
        bool ok = comp.upload_rgba(id, k_width, k_height, first.data()).has_value() &&
                  composite_owned(comp, *target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1) &&
                  comp.upload_rgba(id, k_width, k_height, second.data()).has_value() &&
                  composite_owned(comp, *target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
        out.assign(k_width * k_height * 4u, 0u);
        ok = ok && comp.read_back(*target, out.data(), static_cast<unsigned>(out.size()));
        comp.shutdown();
        return ok;
    };
    std::vector<unsigned char> via_host;
    std::vector<unsigned char> via_staging;
    LUMEN_CHECK(run(gpu.device.context(), via_host));
    lx::gfx::vk_context staged = gpu.device.context();
    staged.supports_host_image_copy = false;
    LUMEN_CHECK(run(staged, via_staging));
    LUMEN_CHECK(via_host == via_staging);
    // The second upload is what shows: a texel the first one did not have.
    const unsigned char* px = via_host.data() + ((10u * k_width) + 20u) * 4u;
    LUMEN_CHECK(px[0] != 0u || px[1] != 0u || px[2] != 0u);
}

// An image larger than the 16 MiB staging ring (a 3200x1800 background was refused: "upload
// exceeds staging ring capacity") goes in bands; a partial update copies only its rects and
// keeps the rest. Both composite to what one whole upload of the expected image does, on the
// staging path and the host-copy path alike.
LUMEN_TEST(shm_upload_oversized_and_partial_regions) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    constexpr unsigned w = 2400, h = 1800; // 17.3 MB of RGBA
    std::vector<unsigned char> base(std::size_t{w} * h * 4u);
    for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
        base[i * 4u + 0] = static_cast<unsigned char>(i % w);
        base[i * 4u + 1] = static_cast<unsigned char>(i / w);
        base[i * 4u + 2] = static_cast<unsigned char>((i % w) ^ (i / w));
        base[i * 4u + 3] = 255u;
    }
    // A damaged region with two rects in it; the region's pixels between them are stale on
    // purpose (zero), so a copy outside the rects would show.
    const lx::rect2i region{100, 1500, 900, 200};
    const lx::rect2i rects[2] = {{100, 1500, 300, 40}, {700, 1650, 300, 50}};
    std::vector<unsigned char> packed(std::size_t{900} * 200 * 4u, 0u);
    std::vector<unsigned char> expected = base;
    for (const lx::rect2i& r : rects) {
        for (int y = r.y; y < r.y + r.height; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                const unsigned char px[4] = {static_cast<unsigned char>(255 - x % 256), 0x40,
                                             static_cast<unsigned char>(y % 256), 255};
                std::memcpy(packed.data() + (std::size_t(y - region.y) * 900 + (x - region.x)) * 4u,
                            px, 4);
                std::memcpy(expected.data() + (std::size_t(y) * w + x) * 4u, px, 4);
            }
        }
    }
    const auto run = [&](lx::gfx::vk_context ctx, bool partial, std::vector<unsigned char>& out) {
        lx::gfx::vulkan_compositor comp;
        if (!comp.initialize(ctx))
            return false;
        auto target = lx::gfx::render_target::create(ctx, w, h);
        if (!target)
            return false;
        constexpr unsigned id = 0x4000'0003u;
        lx::gfx::blit_command cmd{};
        cmd.texture_id = id;
        cmd.dst = {0, 0, static_cast<int>(w), static_cast<int>(h)};
        cmd.opacity = 1.f;
        cmd.blend = lx::blend_mode::opaque;
        bool ok = partial ? comp.upload_rgba(id, w, h, base.data()).has_value() &&
                                comp.upload_rgba(id, w, h, packed.data(), region, rects).has_value()
                          : comp.upload_rgba(id, w, h, expected.data()).has_value();
        ok = ok && composite_owned(comp, *target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
        out.assign(std::size_t{w} * h * 4u, 0u);
        ok = ok && comp.read_back(*target, out.data(), static_cast<unsigned>(out.size()));
        comp.shutdown();
        return ok;
    };
    lx::gfx::vk_context staged = gpu.device.context();
    staged.supports_host_image_copy = false;
    std::vector<unsigned char> reference;
    std::vector<unsigned char> banded;
    LUMEN_CHECK(run(staged, false, reference));
    LUMEN_CHECK(run(staged, true, banded));
    LUMEN_CHECK(reference == banded);
    std::vector<unsigned char> host;
    LUMEN_CHECK(run(gpu.device.context(), true, host));
    LUMEN_CHECK(reference == host);
}

// The one that matters: a real dma-buf, imported through the real importer.
LUMEN_TEST(dmabuf_import_reaches_composited_output) {
#if defined(LUMEN_HAS_DRM)
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    auto source = make_dumb_dmabuf();
    if (!source.valid) {
        std::printf("SKIP: no dumb-buffer dma-buf available on this host\n");
        return;
    }

    lx::gfx::dmabuf_desc desc{};
    desc.width = k_width;
    desc.height = k_height;
    desc.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    desc.plane_count = 1;
    desc.planes[0].stride = source.stride;
    desc.planes[0].offset = 0;
    desc.planes[0].modifier = lx::gfx::modifier_linear;
    // Borrowed: the importer dups before handing ownership to Vulkan.
    desc.planes[0].fd.reset(::dup(source.dmabuf.get()));
    LUMEN_CHECK(desc.planes[0].fd.get() >= 0);

    auto imported = gpu.device.dmabuf().import(desc);
    if (!imported) {
        std::printf("SKIP: driver refused the dma-buf import: %s\n",
                    imported.get_error().message);
        return;
    }
    auto image = std::move(imported).value();
    // A real import must carry a real VkImage — this is what used to be fabricated.
    LUMEN_CHECK(image.vk_image != nullptr);
    LUMEN_CHECK(image.width == k_width);

    LUMEN_CHECK(static_cast<bool>(gpu.compositor.bind_imported(image)));

    lx::gfx::blit_command cmd{};
    cmd.texture_id = image.image_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_skipped == 0);
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                           sizeof(readback))));

    const auto* left = readback + ((k_height / 2u) * k_width + k_width / 4u) * 4u;
    const auto* right = readback + ((k_height / 2u) * k_width + (3u * k_width) / 4u) * 4u;

    // Red on the left, blue on the right — exactly the pattern written into the dma-buf.
    LUMEN_CHECK(left[0] > 200 && left[1] < 60 && left[2] < 60);
    LUMEN_CHECK(right[2] > 200 && right[1] < 60 && right[0] < 60);

    gpu.device.dmabuf().release(image);
#else
    std::printf("SKIP: built without libdrm\n");
#endif
}

// XRGB's fourth byte is undefined: a client may leave it zero. Sampled as alpha, a blended
// draw would add the clear colour under it; it must read as one, as on GL and CPU.
LUMEN_TEST(xrgb_dmabuf_ignores_its_x_byte) {
#if defined(LUMEN_HAS_DRM)
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    auto source = make_dumb_dmabuf(0u);
    if (!source.valid) {
        std::printf("SKIP: no dumb-buffer dma-buf available on this host\n");
        return;
    }

    lx::gfx::dmabuf_desc desc{};
    desc.width = k_width;
    desc.height = k_height;
    desc.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    desc.plane_count = 1;
    desc.planes[0].stride = source.stride;
    desc.planes[0].offset = 0;
    desc.planes[0].modifier = lx::gfx::modifier_linear;
    desc.planes[0].fd.reset(::dup(source.dmabuf.get()));
    LUMEN_CHECK(desc.planes[0].fd.get() >= 0);

    auto imported = gpu.device.dmabuf().import(desc);
    if (!imported) {
        std::printf("SKIP: driver refused the dma-buf import: %s\n",
                    imported.get_error().message);
        return;
    }
    auto image = std::move(imported).value();
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.bind_imported(image)));

    lx::gfx::blit_command cmd{};
    cmd.texture_id = image.image_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.opacity = 1.f;
    cmd.blend = lx::blend_mode::premultiplied;

    auto stats =
        composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 1.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                           sizeof(readback))));
    const auto* left = readback + ((k_height / 2u) * k_width + k_width / 4u) * 4u;
    const auto* right = readback + ((k_height / 2u) * k_width + (3u * k_width) / 4u) * 4u;

    // Red and blue exactly as written: no green from the clear underneath.
    LUMEN_CHECK(left[0] > 200 && left[1] < 60 && left[2] < 60);
    LUMEN_CHECK(right[2] > 200 && right[1] < 60 && right[0] < 60);

    gpu.device.dmabuf().release(image);
#else
    std::printf("SKIP: built without libdrm\n");
#endif
}

// The exported target must be a usable scanout framebuffer, not just an FD.
LUMEN_TEST(render_target_exports_scanout_dmabuf) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    auto exported = gpu.target.export_dmabuf();
    if (!exported) {
        std::printf("SKIP: target export unavailable: %s\n", exported.get_error().message);
        return;
    }
    const auto dmabuf = std::move(exported).value();
    LUMEN_CHECK(dmabuf.fd.get() >= 0);
    LUMEN_CHECK(dmabuf.width == k_width);
    LUMEN_CHECK(dmabuf.height == k_height);
    // Row pitch comes from the driver; it must cover the row, and padding is legal.
    LUMEN_CHECK(dmabuf.stride >= k_width * 4u);
}

// ── Widened draw contract on the GPU path ───────────────────────────────────────────
//
// src / clip / tint were dropped between the scene and the renderer, so viewporter,
// subsurface clipping and alpha-modifier could not reach a pixel. These drive the same
// fields the CPU backend tests cover, through the Vulkan shader instead.

namespace {

/// Uploads a left-half-red / right-half-blue texture and returns its id.
[[nodiscard]] bool upload_split_texture(gpu_fixture& gpu, unsigned texture_id) {
    unsigned char source[k_width * k_height * 4]{};
    for (unsigned y = 0; y < k_height; ++y) {
        for (unsigned x = 0; x < k_width; ++x) {
            unsigned char* px = source + (y * k_width + x) * 4u;
            const bool left = x < k_width / 2u;
            px[0] = left ? 255u : 0u;
            px[1] = 0u;
            px[2] = left ? 0u : 255u;
            px[3] = 255u;
        }
    }
    return static_cast<bool>(
        gpu.compositor.upload_rgba(texture_id, k_width, k_height, source));
}

} // namespace

LUMEN_TEST(src_rect_crops_what_the_shader_samples) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    constexpr unsigned texture_id = 0x4000'0010u;
    if (!upload_split_texture(gpu, texture_id))
        return;

    // Sample only the right (blue) half, stretched over the whole target. Every pixel must
    // come out blue — with src dropped, the left half would still be red.
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.src = {static_cast<int>(k_width / 2u), 0, static_cast<int>(k_width / 2u),
               static_cast<int>(k_height)};
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                            sizeof(readback))));
    for (unsigned y = 0; y < k_height; y += 8) {
        for (unsigned x = 0; x < k_width; x += 8) {
            const unsigned char* px = readback + (y * k_width + x) * 4u;
            LUMEN_CHECK(px[2] > 200u); // blue
            LUMEN_CHECK(px[0] < 64u);  // not red
        }
    }
}

LUMEN_TEST(clip_confines_the_draw_to_a_scissor) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    constexpr unsigned texture_id = 0x4000'0011u;
    if (!upload_split_texture(gpu, texture_id))
        return;

    // Full-target draw, clipped to the top-left quadrant.
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.clip = {0, 0, static_cast<int>(k_width / 2u), static_cast<int>(k_height / 2u)};
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                            sizeof(readback))));
    // Inside the clip: the texture's left half, so red. Outside: the clear color.
    const unsigned char* inside = readback + ((k_height / 4u) * k_width + k_width / 4u) * 4u;
    LUMEN_CHECK(inside[0] > 200u);

    const unsigned char* below =
        readback + ((k_height * 3u / 4u) * k_width + k_width / 4u) * 4u;
    LUMEN_CHECK(below[0] < 32u && below[1] < 32u && below[2] < 32u);
}

LUMEN_TEST(clip_outside_the_target_submits_nothing) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    constexpr unsigned texture_id = 0x4000'0012u;
    if (!upload_split_texture(gpu, texture_id))
        return;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.clip = {static_cast<int>(k_width) + 8, 0, 16, 16};
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 0);
    LUMEN_CHECK(stats.value().draws_culled == 1);
}

LUMEN_TEST(tint_multiplies_the_sampled_color) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    constexpr unsigned texture_id = 0x4000'0013u;
    if (!upload_split_texture(gpu, texture_id))
        return;

    // Drop red entirely; the left half must stop being red rather than merely dim.
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.tint = lx::color::rgb(0.f, 1.f, 1.f, 1.f);
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                            sizeof(readback))));
    const unsigned char* left = readback + ((k_height / 2u) * k_width + k_width / 4u) * 4u;
    LUMEN_CHECK(left[0] < 32u); // red channel killed by the tint
    const unsigned char* right =
        readback + ((k_height / 2u) * k_width + k_width * 3u / 4u) * 4u;
    LUMEN_CHECK(right[2] > 200u); // blue survives
}

// ── Linear-light compositing ────────────────────────────────────────────────────────

// Half-covered white over black must land at linear 0.5, which encodes to sRGB ~188.
// Blending the encoded values instead gives ~128 — visibly darker, and the reason
// antialiased edges grew dark fringes.
LUMEN_TEST(composite_blends_in_linear_light) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    // Premultiplied white at 50% alpha, over an opaque black clear.
    unsigned char source[k_width * k_height * 4]{};
    for (unsigned i = 0; i < k_width * k_height; ++i) {
        unsigned char* px = source + i * 4u;
        px[0] = px[1] = px[2] = 128u;
        px[3] = 128u;
    }

    constexpr unsigned texture_id = 0x4000'0020u;
    if (!gpu.compositor.upload_rgba(texture_id, k_width, k_height, source))
        return;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.blend = lx::blend_mode::premultiplied;
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                            sizeof(readback))));
    const unsigned g = readback[((k_height / 2u) * k_width + k_width / 2u) * 4u + 1u];
    // Source 128/255 decodes to ~0.216 linear, halved by coverage over black, then
    // re-encoded. The number that matters is that it clears the ~110 an encoded-space
    // blend would produce for the same inputs.
    LUMEN_CHECK(g > 120u);
}

// An opaque full-coverage draw must survive the round trip unchanged: no blending happens,
// so the linear pipeline must not shift its values.
LUMEN_TEST(opaque_draw_round_trips_through_the_linear_pipeline) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    unsigned char source[k_width * k_height * 4]{};
    for (unsigned y = 0; y < k_height; ++y) {
        for (unsigned x = 0; x < k_width; ++x) {
            unsigned char* px = source + (y * k_width + x) * 4u;
            px[0] = static_cast<unsigned char>(x * 4u);
            px[1] = static_cast<unsigned char>(y * 4u);
            px[2] = 64u;
            px[3] = 255u;
        }
    }

    constexpr unsigned texture_id = 0x4000'0021u;
    if (!gpu.compositor.upload_rgba(texture_id, k_width, k_height, source))
        return;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.blend = lx::blend_mode::opaque;
    cmd.opacity = 1.f;

    LUMEN_CHECK(static_cast<bool>(
        composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1)));

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                            sizeof(readback))));
    for (unsigned y = 8; y < k_height; y += 16) {
        for (unsigned x = 8; x < k_width; x += 16) {
            const unsigned char* got = readback + (y * k_width + x) * 4u;
            const unsigned char* want = source + (y * k_width + x) * 4u;
            for (unsigned c = 0; c < 3; ++c) {
                const int diff = static_cast<int>(got[c]) - static_cast<int>(want[c]);
                LUMEN_CHECK(diff <= 2 && diff >= -2); // decode/encode rounding only
            }
        }
    }
}

// ── Wide output ─────────────────────────────────────────────────────────────────────
//
// The composite always blends in 16-bit float linear; the encode pass converts into
// whatever the scanout buffer holds. These check that the second half of that actually
// works, at ten bits and with a non-sRGB output curve.

LUMEN_TEST(composite_into_a_ten_bit_scanout_target) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    auto wide = lx::gfx::render_target::create(gpu.device.context(), k_width, k_height,
                                               lx::pixel_format::xrgb2101010);
    if (!wide) {
        std::printf("SKIP: no 10-bit render target: %s\n", wide.get_error().message);
        return;
    }
    // In the fixture's slot (its 8-bit target never drew): destroyed after the compositor
    // waits for the frame that renders into it.
    gpu.target = std::move(wide).value();
    lx::gfx::render_target& target = gpu.target;
    LUMEN_CHECK(target.format() == lx::pixel_format::xrgb2101010);

    constexpr unsigned texture_id = 0x4000'0030u;
    if (!upload_split_texture(gpu, texture_id))
        return;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.blend = lx::blend_mode::opaque;
    cmd.opacity = 1.f;

    auto stats = composite_owned(gpu.compositor, target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);
    LUMEN_CHECK(stats.value().draws_skipped == 0);

    // A 10-bit target is still exportable for scanout — the whole point of rendering into
    // it rather than an offscreen buffer.
    auto exported = target.export_dmabuf();
    LUMEN_CHECK(static_cast<bool>(exported));
    LUMEN_CHECK(exported.value().fd.get() >= 0);
}

// Changing the output transfer function must change the encoded bytes. If it does not, the
// encode pass is not running and the pipeline has quietly become a passthrough.
LUMEN_TEST(output_transfer_function_changes_the_encoded_result) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    // Mid-grey, so the difference between transfer curves is large.
    unsigned char source[k_width * k_height * 4]{};
    for (unsigned i = 0; i < k_width * k_height; ++i) {
        unsigned char* px = source + i * 4u;
        px[0] = px[1] = px[2] = 128u;
        px[3] = 255u;
    }

    constexpr unsigned texture_id = 0x4000'0031u;
    if (!gpu.compositor.upload_rgba(texture_id, k_width, k_height, source))
        return;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.blend = lx::blend_mode::opaque;
    cmd.opacity = 1.f;

    const auto sample_center = [&](lx::transfer_function tf) -> unsigned {
        gpu.compositor.set_output_transfer(tf);
        auto stats = composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
        LUMEN_CHECK(static_cast<bool>(stats));
        static unsigned char readback[k_width * k_height * 4]{};
        LUMEN_CHECK(static_cast<bool>(
            gpu.compositor.read_back(gpu.target, readback, sizeof(readback))));
        return readback[((k_height / 2u) * k_width + k_width / 2u) * 4u + 1u];
    };

    const unsigned as_srgb = sample_center(lx::transfer_function::srgb);
    const unsigned as_linear = sample_center(lx::transfer_function::linear);
    const unsigned as_pq = sample_center(lx::transfer_function::pq);

    // sRGB round-trips the input; linear output leaves the decoded value, which for a
    // mid-grey sRGB input is far darker; PQ is darker still at this signal level.
    LUMEN_CHECK(as_srgb > 100u);
    LUMEN_CHECK(as_linear < as_srgb);
    LUMEN_CHECK(as_pq != as_srgb);

    gpu.compositor.set_output_transfer(lx::transfer_function::srgb);
}

// Present is only non-blocking when the frame's completion can be handed to KMS as a
// sync_file. If the export silently fails the compositor still works — it just blocks the
// render thread on the GPU every frame, which is the thing this was meant to stop. Assert
// the export happens where the device claims to support it, so a regression to the
// blocking path is visible rather than merely slow.
LUMEN_TEST(composite_exports_a_sync_file_when_supported) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;

    if (!gpu.device.info().supports_external_semaphore_fd) {
        std::printf("SKIP: device cannot export sync_files\n");
        return;
    }

    constexpr unsigned texture_id = 0x4000'0040u;
    if (!upload_split_texture(gpu, texture_id))
        return;

    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_width), static_cast<int>(k_height)};
    cmd.opacity = 1.f;

    auto stats = gpu.compositor.composite(gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().out_fence_fd >= 0);
    ::close(stats.value().out_fence_fd);
}

// GPU timing (`set_gpu_timing`): off, a frame is not timed and nothing is reported; on, each
// frame's composite and encode passes come back once its fence has signalled, each non-zero
// and together no longer than the frame took from the composite call to its fence (the
// timestamps run inside that interval), and the pixels are those of an untimed frame.
LUMEN_TEST(gpu_timing_reports_each_frames_passes) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    if (!gpu.compositor.gpu_timing_supported()) {
        std::printf("SKIP: queue family has no timestamps\n");
        return;
    }
    constexpr unsigned texture_id = 0x4000'0050u;
    if (!upload_split_texture(gpu, texture_id))
        return;
    constexpr unsigned k_size = 512;
    auto made = lx::gfx::render_target::create(gpu.device.context(), k_size, k_size);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::render_target target = std::move(made).value();
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(k_size), static_cast<int>(k_size)};
    cmd.scaled = true;
    cmd.opacity = 0.75f;
    const lx::color clear = lx::color::rgb(0.f, 0.2f, 0.f);
    lx::gfx::composite_gpu_time times[lx::gfx::vulkan_compositor::k_gpu_times]{};
    const auto read = [&] {
        std::vector<unsigned char> px(k_size * k_size * 4u);
        LUMEN_CHECK(static_cast<bool>(
            gpu.compositor.read_back(target, px.data(), static_cast<unsigned>(px.size()))));
        return px;
    };

    // Off (the default).
    auto untimed = composite_owned(gpu.compositor, target, clear, &cmd, 1);
    LUMEN_CHECK(untimed && untimed.value().gpu_serial == 0);
    const std::vector<unsigned char> reference = read();
    LUMEN_CHECK(gpu.compositor.take_gpu_times(times, 4) == 0);

    // On: four frames, each waited for through its sync_file (or the composite blocked).
    gpu.compositor.set_gpu_timing(true);
    constexpr unsigned k_frames = 4;
    std::uint64_t serials[k_frames]{};
    std::uint64_t wall_ns[k_frames]{};
    for (unsigned i = 0; i < k_frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto c = gpu.compositor.composite(target, clear, &cmd, 1);
        LUMEN_CHECK(static_cast<bool>(c));
        if (!c)
            return;
        if (c.value().out_fence_fd >= 0) {
            pollfd pfd{c.value().out_fence_fd, POLLIN, 0};
            LUMEN_CHECK(::poll(&pfd, 1, 5000) == 1);
            ::close(c.value().out_fence_fd);
        }
        wall_ns[i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        serials[i] = c.value().gpu_serial;
        LUMEN_CHECK(serials[i] != 0 && (i == 0 || serials[i] > serials[i - 1]));
    }
    // The sync_file signals with the work; the VkFence the results wait for may trail it.
    unsigned got = 0;
    lx::gfx::composite_gpu_time all[k_frames]{};
    for (int tries = 0; tries < 1000 && got < k_frames; ++tries) {
        got += gpu.compositor.take_gpu_times(all + got, k_frames - got);
        if (got < k_frames)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LUMEN_CHECK(got == k_frames);
    for (unsigned i = 0; i < got && i < k_frames; ++i) {
        const std::uint64_t gpu_ns = all[i].composite_ns + all[i].encode_ns;
        std::printf("    frame %u: composite %.3f ms, encode %.3f ms, wall %.3f ms\n", i,
                    all[i].composite_ns / 1e6, all[i].encode_ns / 1e6, wall_ns[i] / 1e6);
        LUMEN_CHECK(all[i].serial == serials[i]);
        LUMEN_CHECK(all[i].composite_ns > 0 && all[i].encode_ns > 0);
        LUMEN_CHECK(gpu_ns <= wall_ns[i]);
    }
    LUMEN_CHECK(read() == reference);
    LUMEN_CHECK(gpu.compositor.take_gpu_times(times, 4) == 0);

    // Off again: untimed.
    gpu.compositor.set_gpu_timing(false);
    auto again = composite_owned(gpu.compositor, target, clear, &cmd, 1);
    LUMEN_CHECK(again && again.value().gpu_serial == 0);
    LUMEN_CHECK(read() == reference);
    LUMEN_CHECK(gpu.compositor.take_gpu_times(times, 4) == 0);
}

// ── solid draws (wp_single_pixel_buffer_v1) ────────────────────────────────

// A solid draw needs no texture of its own: with nothing else in the frame it still binds a
// valid descriptor set (the compositor's 1x1 one) and lands its colour; translucent, it
// blends in linear light like a texel.
LUMEN_TEST(solid_draw_reaches_composited_output_without_a_texture) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    lx::gfx::blit_command cmds[2]{};
    cmds[0].solid = true;
    cmds[0].solid_color = lx::color::rgb(1.f, 0.f, 0.f, 1.f);
    cmds[0].dst = {0, 0, static_cast<int>(k_width) / 2, static_cast<int>(k_height)};
    cmds[0].blend = lx::blend_mode::opaque;
    cmds[1].solid = true;
    cmds[1].solid_color = lx::color::rgb(0.5f, 0.5f, 0.5f, 0.5f);
    cmds[1].dst = {static_cast<int>(k_width) / 2, 0, static_cast<int>(k_width) / 2,
                   static_cast<int>(k_height)};
    cmds[1].blend = lx::blend_mode::premultiplied;
    auto stats =
        composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), cmds, 2);
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 2);
    LUMEN_CHECK(stats.value().draws_skipped == 0);
    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                            sizeof(readback))));
    const unsigned char* left = readback + ((k_height / 2u) * k_width + k_width / 4u) * 4u;
    LUMEN_CHECK(left[0] > 240u && left[1] < 16u && left[2] < 16u);
    // Premultiplied white at 0.5 over black: linear 0.5 encodes to ~188.
    const unsigned char* right = readback + ((k_height / 2u) * k_width + k_width * 3u / 4u) * 4u;
    LUMEN_CHECK(right[1] > 176u && right[1] < 200u);
}

// ── Outputs of different sizes ──────────────────────────────────────────────────────
//
// One compositor draws every output. Each size keeps its own linear intermediate: drawing
// alternately into two sizes must neither reallocate it per frame nor change a pixel.

namespace {

[[nodiscard]] lx::gfx::blit_command fullscreen_blit(unsigned texture_id,
                                                    const lx::gfx::render_target& t) {
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = {0, 0, static_cast<int>(t.width()), static_cast<int>(t.height())};
    cmd.blend = lx::blend_mode::opaque;
    cmd.opacity = 1.f;
    return cmd;
}

} // namespace

LUMEN_TEST(alternating_target_sizes_keep_their_linear_intermediates) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    constexpr unsigned k_small_w = 40;
    constexpr unsigned k_small_h = 24;
    auto made = lx::gfx::render_target::create(gpu.device.context(), k_small_w, k_small_h);
    if (!made) {
        std::printf("SKIP: no second render target: %s\n", made.get_error().message);
        return;
    }
    auto small = std::move(made).value();

    constexpr unsigned texture_id = 0x4000'0050u;
    if (!upload_split_texture(gpu, texture_id))
        return;

    lx::gfx::render_target* targets[2] = {&gpu.target, &small};
    std::vector<unsigned char> first[2];
    const unsigned before = gpu.compositor.linear_allocations();
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned i = 0; i < 2; ++i) {
            lx::gfx::render_target& t = *targets[i];
            const lx::gfx::blit_command cmd = fullscreen_blit(texture_id, t);
            auto stats = composite_owned(gpu.compositor, t, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
            LUMEN_CHECK(static_cast<bool>(stats));
            LUMEN_CHECK(stats.value().draws_submitted == 1);
            // One intermediate per size, made by its first frame and never again.
            LUMEN_CHECK(gpu.compositor.linear_allocations() == before + (round == 0 ? i + 1 : 2));

            std::vector<unsigned char> px(t.width() * t.height() * 4u);
            LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(
                t, px.data(), static_cast<unsigned>(px.size()))));
            const unsigned y = t.height() / 2u;
            const unsigned char* left = px.data() + (y * t.width() + t.width() / 4u) * 4u;
            const unsigned char* right = px.data() + (y * t.width() + t.width() * 3u / 4u) * 4u;
            LUMEN_CHECK(left[0] > 200u && left[1] < 60u && left[2] < 60u);
            LUMEN_CHECK(right[2] > 200u && right[1] < 60u && right[0] < 60u);
            // Every later frame is the first one, byte for byte.
            if (round == 0)
                first[i] = std::move(px);
            else
                LUMEN_CHECK(px == first[i]);
        }
    }
}

// An intermediate whose size has not been drawn for a while (its output went away or
// changed mode) is freed, not kept for good: drawing that size again makes a new one.
LUMEN_TEST(idle_linear_intermediate_is_freed) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    auto made = lx::gfx::render_target::create(gpu.device.context(), 8, 8);
    if (!made) {
        std::printf("SKIP: no second render target: %s\n", made.get_error().message);
        return;
    }
    auto tiny = std::move(made).value();

    lx::gfx::blit_command cmd{};
    cmd.solid = true;
    cmd.solid_color = lx::color::rgb(0.f, 1.f, 0.f, 1.f);
    cmd.dst = {0, 0, 8, 8};
    cmd.blend = lx::blend_mode::opaque;

    const unsigned before = gpu.compositor.linear_allocations();
    LUMEN_CHECK(static_cast<bool>(
        composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1)));
    for (unsigned i = 0; i < 700; ++i)
        LUMEN_CHECK(static_cast<bool>(
            composite_owned(gpu.compositor, tiny, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1)));
    LUMEN_CHECK(gpu.compositor.linear_allocations() == before + 2);
    LUMEN_CHECK(static_cast<bool>(
        composite_owned(gpu.compositor, gpu.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1)));
    LUMEN_CHECK(gpu.compositor.linear_allocations() == before + 3);

    unsigned char readback[k_width * k_height * 4]{};
    LUMEN_CHECK(static_cast<bool>(gpu.compositor.read_back(gpu.target, readback,
                                                            sizeof(readback))));
    LUMEN_CHECK(readback[1] > 240u && readback[0] < 16u && readback[2] < 16u);
}

// ── Damage-limited frames ───────────────────────────────────────────────────────────
//
// `composite_region` composites only the frame's damage into the persistent intermediate
// and encodes damage plus the slot's age into the target. Every frame must equal a full
// repaint of the same scene, byte for byte: windows moving over a scaled wallpaper, a
// translucent one over an opaque one, a translucent solid, a clipped draw, three scanout
// slots in rotation with their ages, a chain broken in the middle (a whole-target black
// frame, as the compositor's power-off fallback draws, into another slot), and a second
// output of the same size drawing its own chain in between.

namespace {

constexpr unsigned k_dmg_w = 96;
constexpr unsigned k_dmg_h = 72;

[[nodiscard]] std::vector<unsigned char> gradient_rgba(unsigned w, unsigned h, unsigned seed,
                                                       bool translucent) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4u);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            unsigned char* p = px.data() + (static_cast<std::size_t>(y) * w + x) * 4u;
            const unsigned a = translucent ? 64u + (x * 191u) / (w - 1u) : 255u;
            // Premultiplied: no channel above alpha.
            p[0] = static_cast<unsigned char>(((x * 37u + seed * 11u) % 256u) * a / 255u);
            p[1] = static_cast<unsigned char>(((y * 53u + seed * 29u) % 256u) * a / 255u);
            p[2] = static_cast<unsigned char>((((x ^ y) * 23u + seed) % 256u) * a / 255u);
            p[3] = static_cast<unsigned char>(a);
        }
    return px;
}

struct dmg_scene {
    lx::gfx::blit_command cmds[5]{};
    unsigned count = 5;
};

[[nodiscard]] bool upload_dmg_textures(lx::gfx::vulkan_compositor& comp) {
    const auto wall = gradient_rgba(16, 16, 1, false);
    const auto a = gradient_rgba(24, 20, 2, false);
    const auto b = gradient_rgba(20, 16, 3, true);
    const auto cursor = gradient_rgba(8, 8, 4, true);
    return comp.upload_rgba(0x4000'0100u, 16, 16, wall.data()) &&
           comp.upload_rgba(0x4000'0101u, 24, 20, a.data()) &&
           comp.upload_rgba(0x4000'0102u, 20, 16, b.data()) &&
           comp.upload_rgba(0x4000'0103u, 8, 8, cursor.data());
}

/// Frame `i` of the scene: the wallpaper (not reaching the bottom), window A (opaque, moving right, clipped every
/// third frame), window B (translucent, scaled, moving left over A), a translucent solid
/// and a cursor.
[[nodiscard]] dmg_scene dmg_frame(unsigned i) {
    dmg_scene s{};
    auto& wall = s.cmds[0];
    wall.texture_id = 0x4000'0100u;
    wall.dst = {0, 0, static_cast<int>(k_dmg_w), 52}; // the clear shows below it
    wall.blend = lx::blend_mode::opaque;
    wall.scaled = true;
    auto& a = s.cmds[1];
    a.texture_id = 0x4000'0101u;
    a.dst = {4 + static_cast<int>(i) * 5, 10, 24, 20};
    a.blend = lx::blend_mode::opaque;
    if (i % 3 == 2)
        a.clip = {a.dst.x + 3, 12, 15, 30};
    auto& b = s.cmds[2];
    b.texture_id = 0x4000'0102u;
    b.dst = {60 - static_cast<int>(i) * 4, 18, 30, 24};
    b.scaled = true;
    b.opacity = 0.75f;
    auto& solid = s.cmds[3];
    solid.solid = true;
    solid.solid_color = lx::color::rgb(0.2f, 0.f, 0.3f, 0.5f);
    solid.dst = {50, 44 + static_cast<int>(i % 4), 30, 10};
    auto& cursor = s.cmds[4];
    cursor.texture_id = 0x4000'0103u;
    cursor.dst = {10 + static_cast<int>(i) * 3, 50 - static_cast<int>(i) * 2, 8, 8};
    return s;
}

/// What moved between two frames: each draw's old and new rect where it changed.
[[nodiscard]] lx::gfx::damage_region dmg_changed(const dmg_scene& from, const dmg_scene& to) {
    lx::gfx::damage_region d{};
    for (unsigned k = 0; k < to.count; ++k) {
        const auto& x = from.cmds[k];
        const auto& y = to.cmds[k];
        if (x.dst.x != y.dst.x || x.dst.y != y.dst.y || x.clip.width != y.clip.width ||
            x.clip.x != y.clip.x) {
            d.add(x.dst);
            d.add(y.dst);
        }
    }
    d.clip({0, 0, static_cast<int>(k_dmg_w), static_cast<int>(k_dmg_h)});
    return d;
}

[[nodiscard]] std::vector<unsigned char> read_rgba(lx::gfx::vulkan_compositor& comp,
                                                   const lx::gfx::render_target& t) {
    std::vector<unsigned char> px(t.width() * t.height() * 4u);
    if (!comp.read_back(t, px.data(), static_cast<unsigned>(px.size())))
        px.clear();
    return px;
}

} // namespace

LUMEN_TEST(damage_limited_frames_match_a_full_repaint) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    lx::gfx::vulkan_compositor ref{};
    if (!ref.initialize(gpu.device.context())) {
        std::printf("SKIP: no second compositor\n");
        return;
    }
    lx::gfx::render_target slots[3]{};
    lx::gfx::render_target other{};
    lx::gfx::render_target ref_target{};
    for (auto* t : {&slots[0], &slots[1], &slots[2], &other, &ref_target}) {
        auto made = lx::gfx::render_target::create(gpu.device.context(), k_dmg_w, k_dmg_h);
        if (!made) {
            std::printf("SKIP: no render target: %s\n", made.get_error().message);
            return;
        }
        *t = std::move(made).value();
    }
    LUMEN_CHECK(upload_dmg_textures(gpu.compositor));
    LUMEN_CHECK(upload_dmg_textures(ref));
    const lx::color clear = lx::color::rgb(0.05f, 0.1f, 0.15f);

    lx::gfx::damage_region age[3]{};
    for (auto& a : age)
        a.set_full();
    const lx::gfx::render_target* previous = nullptr;
    dmg_scene last = dmg_frame(0);
    const lx::gfx::render_target* other_previous = nullptr;
    dmg_scene other_last = dmg_frame(13);
    bool partial_seen = false;
    for (unsigned i = 0; i < 14; ++i) {
        const dmg_scene scene = dmg_frame(i);
        lx::gfx::damage_region changed{};
        if (i == 0)
            changed.set_full();
        else
            changed = dmg_changed(last, scene);
        const unsigned back = i % 3u;
        if (i == 8) {
            // The chain broken: a black frame into another slot, every slot's age full. After
            // the second output's frame, so it takes this output's intermediate (the older).
            auto black = gpu.compositor.composite(slots[(back + 1u) % 3u],
                                                  lx::color::rgb(0.f, 0.f, 0.f), nullptr, 0);
            LUMEN_CHECK(static_cast<bool>(black));
            if (black && black.value().out_fence_fd >= 0)
                ::close(black.value().out_fence_fd);
            for (auto& a : age)
                a.set_full();
        }
        lx::gfx::damage_region region = changed;
        region.add_region(age[back]);
        auto drawn = gpu.compositor.composite_region(slots[back], clear, scene.cmds, scene.count,
                                                     region, changed, previous);
        LUMEN_CHECK(static_cast<bool>(drawn));
        if (!drawn)
            return;
        if (drawn.value().out_fence_fd >= 0)
            ::close(drawn.value().out_fence_fd);
        if (drawn.value().draws_culled > 0)
            partial_seen = true; // a draw met no damage rect: the pass was limited
        for (unsigned s = 0; s < 3; ++s)
            if (s != back)
                age[s].add_region(changed);
        age[back].clear();
        previous = &slots[back];
        last = scene;

        auto r = composite_owned(ref, ref_target, clear, scene.cmds, scene.count);
        LUMEN_CHECK(static_cast<bool>(r));
        const auto want = read_rgba(ref, ref_target);
        const auto got = read_rgba(gpu.compositor, slots[back]);
        LUMEN_CHECK(!want.empty() && got == want);

        // A second output of the same size, its own chain (one slot), every other frame.
        if (i % 2 == 1) {
            const dmg_scene o = dmg_frame(13 - i);
            lx::gfx::damage_region ochanged = dmg_changed(other_last, o);
            if (!other_previous)
                ochanged.set_full();
            auto od = gpu.compositor.composite_region(other, clear, o.cmds, o.count, ochanged,
                                                      ochanged, other_previous);
            LUMEN_CHECK(static_cast<bool>(od));
            if (od && od.value().out_fence_fd >= 0)
                ::close(od.value().out_fence_fd);
            other_previous = &other;
            other_last = o;
            auto ro = composite_owned(ref, ref_target, clear, o.cmds, o.count);
            LUMEN_CHECK(static_cast<bool>(ro));
            const auto owant = read_rgba(ref, ref_target);
            LUMEN_CHECK(!owant.empty() && read_rgba(gpu.compositor, other) == owant);
        }
    }
    LUMEN_CHECK(partial_seen);
    // The second output took an intermediate of its own rather than breaking the first
    // output's chain every other frame.
    LUMEN_CHECK(gpu.compositor.linear_allocations() == 2);
    ref.shutdown();
}

// `blend_mode::opaque` draws take a pipeline without blending (as GL and CPU draw them).
// Content of alpha 1 replaces the destination either way: every byte must match the same
// frame drawn with every draw blended.
LUMEN_TEST(opaque_draws_without_blending_match_blended_ones) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    lx::gfx::render_target blended{};
    lx::gfx::render_target plain{};
    for (auto* t : {&blended, &plain}) {
        auto made = lx::gfx::render_target::create(gpu.device.context(), k_dmg_w, k_dmg_h);
        if (!made) {
            std::printf("SKIP: no render target: %s\n", made.get_error().message);
            return;
        }
        *t = std::move(made).value();
    }
    LUMEN_CHECK(upload_dmg_textures(gpu.compositor));
    const lx::color clear = lx::color::rgb(0.05f, 0.1f, 0.15f);
    for (unsigned i = 0; i < 4; ++i) {
        dmg_scene s = dmg_frame(i);
        // An opaque solid too (a single-pixel buffer of alpha 1).
        s.cmds[3].solid_color = lx::color::rgb(0.2f, 0.f, 0.3f, 1.f);
        s.cmds[3].blend = lx::blend_mode::opaque;
        dmg_scene all_blended = s;
        unsigned opaque = 0;
        for (unsigned k = 0; k < all_blended.count; ++k) {
            opaque += all_blended.cmds[k].blend == lx::blend_mode::opaque ? 1u : 0u;
            all_blended.cmds[k].blend = lx::blend_mode::premultiplied;
        }
        LUMEN_CHECK(opaque == 3);
        LUMEN_CHECK(static_cast<bool>(
            composite_owned(gpu.compositor, plain, clear, s.cmds, s.count)));
        LUMEN_CHECK(static_cast<bool>(composite_owned(gpu.compositor, blended, clear,
                                                      all_blended.cmds, all_blended.count)));
        const auto want = read_rgba(gpu.compositor, blended);
        LUMEN_CHECK(!want.empty() && read_rgba(gpu.compositor, plain) == want);
    }
    // It is another pipeline: a translucent draw marked opaque replaces what is under it.
    const dmg_scene s = dmg_frame(0);
    dmg_scene marked = s;
    marked.cmds[2].blend = lx::blend_mode::opaque;
    LUMEN_CHECK(static_cast<bool>(
        composite_owned(gpu.compositor, plain, clear, marked.cmds, marked.count)));
    LUMEN_CHECK(static_cast<bool>(composite_owned(gpu.compositor, blended, clear, s.cmds, s.count)));
    const auto want = read_rgba(gpu.compositor, blended);
    LUMEN_CHECK(!want.empty() && read_rgba(gpu.compositor, plain) != want);
}

LUMEN_TEST(visible_rects_match_whole_draws) {
    gpu_fixture gpu{};
    if (!setup_gpu(gpu))
        return;
    lx::gfx::render_target whole_t{};
    lx::gfx::render_target cut_t{};
    for (auto* t : {&whole_t, &cut_t}) {
        auto made = lx::gfx::render_target::create(gpu.device.context(), k_dmg_w, k_dmg_h);
        if (!made) {
            std::printf("SKIP: no render target: %s\n", made.get_error().message);
            return;
        }
        *t = std::move(made).value();
    }
    LUMEN_CHECK(upload_dmg_textures(gpu.compositor));
    // A scaled wallpaper; a translucent scaled window and a translucent solid, both partly
    // under an opaque window; a cursor on top. The cut copy draws only the visible rects
    // (the solid's an over-cover: it may draw more than shows, never less).
    lx::gfx::blit_command whole[5]{};
    whole[0].texture_id = 0x4000'0100u;
    whole[0].dst = {0, 0, 64, 48};
    whole[0].blend = lx::blend_mode::opaque;
    whole[0].scaled = true;
    whole[1].texture_id = 0x4000'0102u;
    whole[1].dst = {8, 6, 40, 30};
    whole[1].opacity = 0.75f;
    whole[1].scaled = true;
    whole[2].solid = true;
    whole[2].solid_color = lx::color::rgb(0.2f, 0.f, 0.3f, 0.5f);
    whole[2].dst = {30, 30, 30, 12};
    whole[3].texture_id = 0x4000'0101u;
    whole[3].dst = {20, 14, 24, 20};
    whole[3].blend = lx::blend_mode::opaque;
    whole[4].texture_id = 0x4000'0103u;
    whole[4].dst = {40, 30, 8, 8};
    lx::gfx::blit_command cut[5]{};
    for (unsigned i = 0; i < 5; ++i)
        cut[i] = whole[i];
    const lx::rect2i wall_vis[4] = {{0, 0, 64, 14}, {0, 34, 64, 14}, {0, 14, 20, 20}, {44, 14, 20, 20}};
    const lx::rect2i win_vis[4] = {{8, 6, 40, 8}, {8, 34, 40, 2}, {8, 14, 12, 20}, {44, 14, 4, 20}};
    for (unsigned k = 0; k < 4; ++k) {
        cut[0].visible[k] = wall_vis[k];
        cut[1].visible[k] = win_vis[k];
    }
    cut[0].visible_count = 4;
    cut[1].visible_count = 4;
    cut[2].visible[0] = {30, 32, 30, 10};
    cut[2].visible[1] = {44, 30, 16, 2};
    cut[2].visible_count = 2;
    const lx::color clear = lx::color::rgb(0.05f, 0.1f, 0.15f);
    auto a = composite_owned(gpu.compositor, whole_t, clear, whole, 5);
    auto b = composite_owned(gpu.compositor, cut_t, clear, cut, 5);
    LUMEN_CHECK(a && b && a.value().draws_submitted == 5 && b.value().draws_submitted == 5);
    const auto want = read_rgba(gpu.compositor, whole_t);
    LUMEN_CHECK(!want.empty() && read_rgba(gpu.compositor, cut_t) == want);
    // A draw none of whose visible rects reach the target's pixels is not recorded.
    cut[1].visible[0] = {200, 200, 4, 4};
    cut[1].visible_count = 1;
    auto c = composite_owned(gpu.compositor, cut_t, clear, cut, 5);
    LUMEN_CHECK(c && c.value().draws_culled == 1 && c.value().draws_submitted == 4);
    // Waited for before the targets go.
    LUMEN_CHECK(!read_rgba(gpu.compositor, cut_t).empty());
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
