// Hardware GL composite test. Runs against a real render node, so it verifies the whole
// EGL/GBM chain the compositor's GL present path depends on: surfaceless context, gbm_bo
// as a GL framebuffer, shader rect math, blending, and scanout export.
//
// Skips (passes) when there is no usable GPU — a machine without /dev/dri or with only a
// software rasterizer has nothing to say about this path.
#include "lumen_test.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#if defined(LUMEN_HAS_EGL)
#include <GLES2/gl2.h>
#endif

import lx.foundation;
import lx.gfx;

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);

int g_drm_fd = -1;

/// Opens a render node once for the whole run. Returns -1 when none is usable.
int render_node() {
    static bool tried = false;
    if (!tried) {
        tried = true;
        g_drm_fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    }
    return g_drm_fd;
}

/// Non-null only when EGL comes up on real hardware. Owned for the process lifetime: an
/// EGL context is per-thread, and rebuilding one per test would be pure overhead.
lx::gfx::egl_device* shared_device() {
    static lx::gfx::egl_device device{};
    static bool initialized = false;
    static bool usable = false;
    if (!initialized) {
        initialized = true;
        const int fd = render_node();
        if (fd < 0)
            return nullptr;
        auto created = lx::gfx::egl_device::create(fd);
        if (!created) {
            std::printf("  (skip: %s)\n", created.get_error().message);
            return nullptr;
        }
        device = std::move(created).value();
        if (device.is_software_renderer()) {
            std::printf("  (skip: GL renderer is software — %s)\n", device.renderer());
            return nullptr;
        }
        usable = true;
    }
    return usable ? &device : nullptr;
}

lx::gfx::blit_command quad(unsigned texture_id, lx::rect2i dst,
                           lx::blend_mode blend = lx::blend_mode::opaque, float opacity = 1.f) {
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = dst;
    cmd.blend = blend;
    cmd.opacity = opacity;
    return cmd;
}

/// Solid RGBA source pixels.
std::vector<unsigned char> solid(unsigned w, unsigned h, unsigned char r, unsigned char g,
                                 unsigned char b, unsigned char a = 255) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}

struct pixel {
    unsigned char r, g, b, a;
};

pixel at(const std::vector<unsigned char>& rgba, unsigned width, unsigned x, unsigned y) {
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

/// `composite` hands back an owned sync_file; tests must close it like the compositor does.
void close_fence(const lx::result<lx::gfx::gl_composite_stats>& stats) {
    if (stats && stats.value().out_fence_fd >= 0)
        ::close(stats.value().out_fence_fd);
}

bool near(unsigned char got, unsigned char want, int tolerance = 4) {
    const int diff = static_cast<int>(got) - static_cast<int>(want);
    return (diff < 0 ? -diff : diff) <= tolerance;
}

} // namespace

LUMEN_TEST(gl_device_reports_a_hardware_renderer) {
    auto* device = shared_device();
    if (!device)
        return;
    LUMEN_CHECK(device->valid());
    LUMEN_CHECK(device->renderer()[0] != '\0');
    std::printf("  GL renderer: %s\n", device->renderer());
    // Every driver worth taking this path for exposes it; without it, client windows would
    // have to be uploaded instead of sampled in place.
    LUMEN_CHECK(device->supports_dmabuf_import());
}

LUMEN_TEST(gl_scanout_target_is_framebuffer_complete_and_exportable) {
    auto* device = shared_device();
    if (!device)
        return;

    auto target = lx::gfx::gl_scanout_target::create(*device, 256, 128, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));
    LUMEN_CHECK(target.value().valid());
    LUMEN_CHECK(target.value().width() == 256);
    LUMEN_CHECK(target.value().height() == 128);

    // Without a working export the composite result could never reach KMS.
    auto exported = target.value().export_dmabuf();
    LUMEN_CHECK(static_cast<bool>(exported));
    LUMEN_CHECK(exported.value().fd.get() >= 0);
    LUMEN_CHECK(exported.value().stride >= 256u * 4u);
}

LUMEN_TEST(gl_composite_clears_and_draws_an_uploaded_texture) {
    auto* device = shared_device();
    if (!device)
        return;

    constexpr unsigned W = 64, H = 32;
    auto target = lx::gfx::gl_scanout_target::create(*device, W, H, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto red = solid(8, 8, 255, 0, 0);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 8, 8, red.data())));

    // Green clear, with the red texture covering the left half.
    const auto cmd = quad(1, {0, 0, static_cast<int>(W) / 2, static_cast<int>(H)});
    auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 1.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);
    LUMEN_CHECK(stats.value().draws_skipped == 0);
    close_fence(stats);

    std::vector<unsigned char> out(static_cast<std::size_t>(W) * H * 4);
    LUMEN_CHECK(static_cast<bool>(
        comp.read_back(target.value(), out.data(), static_cast<unsigned>(out.size()))));

    // Left half is the texture; right half is the clear. This also pins the orientation:
    // row 0 must be the top row, matching every other surface in the compositor.
    const pixel left = at(out, W, 4, 4);
    LUMEN_CHECK(near(left.r, 255) && near(left.g, 0) && near(left.b, 0));
    const pixel right = at(out, W, W - 4, 4);
    LUMEN_CHECK(near(right.r, 0) && near(right.g, 255) && near(right.b, 0));
}

// What the display shows: row 0 of the scanout buffer is the top line. read_back alone
// cannot pin orientation — a flip in the composite and a matching
// flip in the read-back agree with each other and both disagree with the screen, which is
// exactly how every frame once went out upside down with these tests green.
LUMEN_TEST(gl_composite_scanout_memory_row_0_is_the_top) {
    auto* device = shared_device();
    if (!device)
        return;

    constexpr unsigned W = 64, H = 32;
    auto target = lx::gfx::gl_scanout_target::create(*device, W, H, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));
    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto red = solid(8, 8, 255, 0, 0);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 8, 8, red.data())));
    // Green clear with a red band across the top eight rows.
    const auto cmd = quad(1, {0, 0, static_cast<int>(W), 8});
    auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 1.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    close_fence(stats);

    // read_back synchronises with the GPU, and must agree with the display.
    std::vector<unsigned char> out(static_cast<std::size_t>(W) * H * 4);
    LUMEN_CHECK(static_cast<bool>(
        comp.read_back(target.value(), out.data(), static_cast<unsigned>(out.size()))));
    const pixel rb_top = at(out, W, W / 2, 2);
    LUMEN_CHECK(near(rb_top.r, 255) && near(rb_top.g, 0));

#if defined(LUMEN_HAS_EGL)
    // Independent of read_back, which a matching flip would make agree with a wrong
    // composite. Raw glReadPixels on this framebuffer object returns window y = 0 first,
    // and that is the order the display shows: with the old y-negating shader the screen
    // was upside down, and so was this raw read (the old read_back reversed it to hide
    // that). Row 0 here is the top line of the screen.
    std::vector<unsigned char> raw(static_cast<std::size_t>(W) * H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, target.value().framebuffer());
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LUMEN_CHECK(glGetError() == GL_NO_ERROR);
    const pixel top = at(raw, W, W / 2, 2);
    const pixel bottom = at(raw, W, W / 2, H - 3);
    LUMEN_CHECK(near(top.r, 255) && near(top.g, 0) && near(top.b, 0));
    LUMEN_CHECK(near(bottom.r, 0) && near(bottom.g, 255) && near(bottom.b, 0));
#endif
}

LUMEN_TEST(gl_composite_places_a_draw_at_its_destination_rect) {
    auto* device = shared_device();
    if (!device)
        return;

    constexpr unsigned W = 64, H = 64;
    auto target = lx::gfx::gl_scanout_target::create(*device, W, H, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto blue = solid(4, 4, 0, 0, 255);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 4, 4, blue.data())));

    // A quad in the top-left quadrant only — catches a flipped or mistransformed rect,
    // which a fullscreen draw would hide.
    const auto cmd = quad(1, {8, 8, 16, 16});
    {
        auto drawn = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
        LUMEN_CHECK(static_cast<bool>(drawn));
        close_fence(drawn);
    }

    std::vector<unsigned char> out(static_cast<std::size_t>(W) * H * 4);
    LUMEN_CHECK(static_cast<bool>(
        comp.read_back(target.value(), out.data(), static_cast<unsigned>(out.size()))));

    const pixel inside = at(out, W, 16, 16);
    LUMEN_CHECK(near(inside.b, 255) && near(inside.r, 0));
    // Just outside every edge stays cleared.
    LUMEN_CHECK(near(at(out, W, 16, 4).b, 0));
    LUMEN_CHECK(near(at(out, W, 16, 30).b, 0));
    LUMEN_CHECK(near(at(out, W, 4, 16).b, 0));
    LUMEN_CHECK(near(at(out, W, 30, 16).b, 0));
}

LUMEN_TEST(gl_composite_blends_premultiplied_alpha) {
    auto* device = shared_device();
    if (!device)
        return;

    constexpr unsigned W = 32, H = 32;
    auto target = lx::gfx::gl_scanout_target::create(*device, W, H, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    // Half-transparent premultiplied red over a white clear: red stays, green/blue halve.
    const auto half_red = solid(4, 4, 128, 0, 0, 128);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 4, 4, half_red.data())));

    const auto cmd = quad(1, {0, 0, static_cast<int>(W), static_cast<int>(H)},
                          lx::blend_mode::premultiplied);
    {
        auto drawn = comp.composite(target.value(), lx::color::rgb(1.f, 1.f, 1.f), &cmd, 1, {});
        LUMEN_CHECK(static_cast<bool>(drawn));
        close_fence(drawn);
    }

    std::vector<unsigned char> out(static_cast<std::size_t>(W) * H * 4);
    LUMEN_CHECK(static_cast<bool>(
        comp.read_back(target.value(), out.data(), static_cast<unsigned>(out.size()))));

    const pixel p = at(out, W, 16, 16);
    LUMEN_CHECK(near(p.r, 255, 8));
    LUMEN_CHECK(near(p.g, 127, 8));
    LUMEN_CHECK(near(p.b, 127, 8));
}

LUMEN_TEST(gl_composite_scissors_to_the_damage_rect) {
    auto* device = shared_device();
    if (!device)
        return;

    constexpr unsigned W = 64, H = 64;
    auto target = lx::gfx::gl_scanout_target::create(*device, W, H, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto white = solid(4, 4, 255, 255, 255);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 4, 4, white.data())));

    // Paint the whole target red first.
    const auto full = quad(1, {0, 0, static_cast<int>(W), static_cast<int>(H)});
    {
        auto drawn = comp.composite(target.value(), lx::color::rgb(1.f, 0.f, 0.f), nullptr, 0, {});
        LUMEN_CHECK(static_cast<bool>(drawn));
        close_fence(drawn);
    }

    // Now a damaged region only: outside it must keep the previous frame's contents.
    const lx::rect2i damage{0, 0, 16, 16};
    {
        auto drawn = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 1.f), &full, 1, damage);
        LUMEN_CHECK(static_cast<bool>(drawn));
        close_fence(drawn);
    }

    std::vector<unsigned char> out(static_cast<std::size_t>(W) * H * 4);
    LUMEN_CHECK(static_cast<bool>(
        comp.read_back(target.value(), out.data(), static_cast<unsigned>(out.size()))));

    // Inside the damage: the white draw landed.
    const pixel inside = at(out, W, 8, 8);
    LUMEN_CHECK(near(inside.r, 255) && near(inside.g, 255) && near(inside.b, 255));
    // Outside: still the first frame's red clear, not the second frame's blue.
    const pixel outside = at(out, W, 40, 40);
    LUMEN_CHECK(near(outside.r, 255) && near(outside.b, 0));
}

LUMEN_TEST(gl_composite_reports_a_draw_whose_texture_is_missing) {
    auto* device = shared_device();
    if (!device)
        return;

    auto target = lx::gfx::gl_scanout_target::create(*device, 32, 32, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto cmd = quad(99, {0, 0, 32, 32});
    auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_skipped == 1);
    LUMEN_CHECK(stats.value().draws_submitted == 0);
    close_fence(stats);
}

LUMEN_TEST(gl_compositor_forgets_textures) {
    auto* device = shared_device();
    if (!device)
        return;

    auto target = lx::gfx::gl_scanout_target::create(*device, 32, 32, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto px = solid(4, 4, 10, 20, 30);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(7, 4, 4, px.data())));
    comp.forget_texture(7);

    const auto cmd = quad(7, {0, 0, 32, 32});
    auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_skipped == 1);
    close_fence(stats);
}

/// Re-uploading the same texture every frame is the shm client's steady state; it must not
/// leak GL names or start failing.
LUMEN_TEST(gl_compositor_survives_repeated_uploads) {
    auto* device = shared_device();
    if (!device)
        return;

    constexpr unsigned W = 32, H = 32;
    auto target = lx::gfx::gl_scanout_target::create(*device, W, H, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto cmd = quad(1, {0, 0, static_cast<int>(W), static_cast<int>(H)});
    for (unsigned frame = 0; frame < 32; ++frame) {
        const auto px = solid(16, 16, static_cast<unsigned char>(frame * 8), 0, 0);
        LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 16, 16, px.data())));
        auto drawn = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
        LUMEN_CHECK(static_cast<bool>(drawn));
        close_fence(drawn);
    }

    std::vector<unsigned char> out(static_cast<std::size_t>(W) * H * 4);
    LUMEN_CHECK(static_cast<bool>(
        comp.read_back(target.value(), out.data(), static_cast<unsigned>(out.size()))));
    LUMEN_CHECK(near(at(out, W, 16, 16).r, 31 * 8, 8));
}

/// Without this the fence path could regress to the glFinish fallback unnoticed — the
/// pixels would still be right and every other test would still pass.
LUMEN_TEST(gl_composite_exports_a_completion_fence) {
    auto* device = shared_device();
    if (!device)
        return;
    if (!device->supports_native_fence()) {
        std::printf("  (skip: EGL_ANDROID_native_fence_sync unavailable)\n");
        return;
    }

    auto target = lx::gfx::gl_scanout_target::create(*device, 64, 64, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));

    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    const auto px = solid(8, 8, 0, 0, 255);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 8, 8, px.data())));

    const auto cmd = quad(1, {0, 0, 64, 64});
    auto stats = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().out_fence_fd >= 0);
    close_fence(stats);
}

// Server-side frames fill rects by stretching the centre 2x2 of a 4x4 swatch in an atlas
// (lx.compositor:decoration). The half-texel inset must keep linear filtering inside the
// swatch: no bleed from the neighbouring swatch at any edge of a large stretch.
LUMEN_TEST(gl_composite_stretches_a_swatch_centre_without_bleed) {
    auto* device = shared_device();
    if (!device)
        return;
    constexpr unsigned W = 64, H = 32;
    auto target = lx::gfx::gl_scanout_target::create(*device, W, H, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(target));
    lx::gfx::gl_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));

    // 8x4: a dark grey swatch, then a bright red one right beside it.
    std::vector<unsigned char> atlas(8 * 4 * 4);
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            unsigned char* p = &atlas[(y * 8 + x) * 4];
            const bool red = x >= 4;
            p[0] = red ? 0xE0 : 0x30;
            p[1] = red ? 0x44 : 0x30;
            p[2] = red ? 0x3E : 0x33;
            p[3] = 0xFF;
        }
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, 8, 4, atlas.data())));

    lx::gfx::blit_command cmds[2] = {quad(1, {0, 0, 40, 16}), quad(1, {0, 16, 64, 16})};
    cmds[0].src = {1, 1, 2, 2}; // grey centre
    cmds[1].src = {5, 1, 2, 2}; // red centre
    {
        auto drawn = comp.composite(target.value(), lx::color::rgb(0.f, 0.f, 0.f), cmds, 2, {});
        LUMEN_CHECK(static_cast<bool>(drawn));
        close_fence(drawn);
    }
    std::vector<unsigned char> out(static_cast<std::size_t>(W) * H * 4);
    LUMEN_CHECK(static_cast<bool>(
        comp.read_back(target.value(), out.data(), static_cast<unsigned>(out.size()))));
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 40; ++x) {
            const pixel p = at(out, W, x, y);
            LUMEN_CHECK(near(p.r, 0x30, 1) && near(p.g, 0x30, 1) && near(p.b, 0x33, 1));
        }
    for (unsigned y = 16; y < 32; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            const pixel p = at(out, W, x, y);
            LUMEN_CHECK(near(p.r, 0xE0, 1) && near(p.g, 0x44, 1) && near(p.b, 0x3E, 1));
        }
}

int main(int argc, char** argv) {
    const int rc = lumen_test::run_all(argc, argv);
    if (g_drm_fd >= 0)
        ::close(g_drm_fd);
    return rc;
}
