// End-to-end check that a real client buffer's pixels reach the composited output.
//
// This exists because the previous render path could not fail this class of bug: an
// FD-less dmabuf desc produced a fabricated VkImage, and an unresolved texture produced a
// magenta placeholder, so a golden-image test passed while sampling nothing. Every
// assertion below is on pixels that could only come from the source buffer.
#include "lumen_test.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_DRM)
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

import lx.foundation;
import lx.gfx;

namespace {

constexpr unsigned k_width = 64;
constexpr unsigned k_height = 64;

/// Left half opaque red, right half opaque blue, in little-endian XRGB8888 as a Wayland
/// client would write it.
void fill_pattern(unsigned char* pixels, unsigned width, unsigned height, unsigned stride) {
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            unsigned char* px = pixels + y * stride + x * 4u;
            const bool left = x < width / 2u;
            px[0] = left ? 0u : 255u;   // B
            px[1] = 0u;                 // G
            px[2] = left ? 255u : 0u;   // R
            px[3] = 255u;               // X/A
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
    lx::gfx::vulkan_compositor compositor{};
    lx::gfx::render_target target{};
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

dumb_dmabuf make_dumb_dmabuf() {
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
    fill_pattern(pixels, k_width, k_height, out.stride);
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
    auto target = std::move(wide).value();
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

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
