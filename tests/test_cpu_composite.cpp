#include "lumen_test.hpp"

#include <cstdint>
#include <vector>

import lx.foundation;
import lx.gfx;

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
constexpr auto k_argb = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
constexpr auto k_rgba = static_cast<lx::fourcc>(lx::pixel_format::rgba8888);

struct image {
    std::vector<std::uint32_t> pixels{};
    unsigned width = 0;
    unsigned height = 0;
    lx::fourcc format = 0;

    image(unsigned w, unsigned h, lx::fourcc f, std::uint32_t fill = 0)
        : pixels(static_cast<std::size_t>(w) * h, fill), width{w}, height{h}, format{f} {}

    [[nodiscard]] lx::gfx::pixel_surface surface() {
        lx::gfx::pixel_surface s{};
        s.pixels = reinterpret_cast<unsigned char*>(pixels.data());
        s.width = width;
        s.height = height;
        s.stride = width * 4u;
        s.format = format;
        return s;
    }

    [[nodiscard]] std::uint32_t at(unsigned x, unsigned y) const {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
};

lx::gfx::blit_command quad(unsigned texture_id, lx::rect2i dst,
                           lx::blend_mode blend = lx::blend_mode::opaque, float opacity = 1.f) {
    lx::gfx::blit_command cmd{};
    cmd.texture_id = texture_id;
    cmd.dst = dst;
    cmd.blend = blend;
    cmd.opacity = opacity;
    return cmd;
}

} // namespace

LUMEN_TEST(cpu_composite_unscaled_same_format_copies_exactly) {
    image src{8, 4, k_xrgb};
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 8; ++x)
            src.pixels[y * 8 + x] = 0xFF000000u | (x << 8) | y;

    image dst{8, 4, k_xrgb, 0x00112233u};

    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));

    const auto cmd = quad(1, {0, 0, 8, 4});
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);

    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 8; ++x)
            LUMEN_CHECK(dst.at(x, y) == src.at(x, y));
}

LUMEN_TEST(cpu_composite_fullscreen_opaque_draw_skips_the_clear) {
    image src{4, 4, k_xrgb, 0xFF203040u};
    image dst{4, 4, k_xrgb};

    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));

    // A background draw below a covering opaque draw is invisible: neither it nor the
    // clear underneath should cost a pass over the frame.
    const lx::gfx::blit_command cmds[2] = {quad(1, {0, 0, 4, 4}), quad(1, {0, 0, 4, 4})};
    auto stats = comp.composite(dst.surface(), lx::color::rgb(1.f, 0.f, 0.f), cmds, 2, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().cleared == false);
    LUMEN_CHECK(stats.value().draws_culled == 1);
    LUMEN_CHECK(stats.value().draws_submitted == 1);
    LUMEN_CHECK(stats.value().bytes_written == 4ull * 4ull * 4ull);
    LUMEN_CHECK(dst.at(0, 0) == 0xFF203040u);
}

LUMEN_TEST(cpu_composite_partial_draw_clears_only_the_damage) {
    image src{2, 2, k_xrgb, 0xFF00FF00u};
    image dst{8, 8, k_xrgb, 0xDEADBEEFu};

    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));

    const auto cmd = quad(1, {2, 2, 2, 2});
    const lx::rect2i damage{2, 2, 2, 2};
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, damage);
    LUMEN_CHECK(static_cast<bool>(stats));

    // Inside the damage rect the draw landed; everything else keeps its old contents.
    LUMEN_CHECK(dst.at(2, 2) == 0xFF00FF00u);
    LUMEN_CHECK(dst.at(3, 3) == 0xFF00FF00u);
    LUMEN_CHECK(dst.at(0, 0) == 0xDEADBEEFu);
    LUMEN_CHECK(dst.at(7, 7) == 0xDEADBEEFu);
    LUMEN_CHECK(dst.at(4, 2) == 0xDEADBEEFu);
}

LUMEN_TEST(cpu_composite_swaps_red_and_blue_between_channel_orders) {
    // Memory bytes R,G,B,A = 0x10,0x20,0x30,0xFF.
    image src{1, 1, k_rgba, 0xFF302010u};
    image dst{1, 1, k_xrgb};

    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));

    const auto cmd = quad(1, {0, 0, 1, 1});
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_submitted == 1);
    // Same colour in B,G,R,A order.
    LUMEN_CHECK(dst.at(0, 0) == 0xFF102030u);
}

LUMEN_TEST(cpu_composite_blends_premultiplied_alpha_over_the_background) {
    image src{1, 1, k_argb, 0x80402010u}; // premultiplied, alpha 0x80
    image dst{1, 1, k_xrgb, 0xFF000000u};

    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));

    const auto cmd = quad(1, {0, 0, 1, 1}, lx::blend_mode::premultiplied);
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));

    // src + dst * (1 - 0.5): the source channels survive, the opaque black adds nothing to
    // R/G/B, and the alpha climbs back to fully opaque.
    const std::uint32_t out = dst.at(0, 0);
    LUMEN_CHECK((out & 0x00FFFFFFu) == 0x00402010u);
    LUMEN_CHECK((out >> 24) >= 0xFEu);
}

LUMEN_TEST(cpu_composite_scales_a_smaller_source_across_the_destination) {
    image src{2, 1, k_xrgb};
    src.pixels[0] = 0xFF0000FFu;
    src.pixels[1] = 0xFFFF0000u;

    image dst{4, 1, k_xrgb};

    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));

    const auto cmd = quad(1, {0, 0, 4, 1});
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(dst.at(0, 0) == 0xFF0000FFu);
    LUMEN_CHECK(dst.at(1, 0) == 0xFF0000FFu);
    LUMEN_CHECK(dst.at(2, 0) == 0xFFFF0000u);
    LUMEN_CHECK(dst.at(3, 0) == 0xFFFF0000u);
}

LUMEN_TEST(cpu_composite_reports_a_draw_whose_texture_is_missing) {
    image dst{4, 4, k_xrgb};

    lx::gfx::cpu_compositor comp;
    const auto cmd = quad(7, {0, 0, 4, 4});
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_skipped == 1);
    LUMEN_CHECK(stats.value().draws_submitted == 0);
    // Nothing drawable means the clear still has to happen.
    LUMEN_CHECK(stats.value().cleared == true);
}

LUMEN_TEST(cpu_composite_forget_removes_the_texture) {
    image src{2, 2, k_xrgb, 0xFF112233u};
    image dst{2, 2, k_xrgb};

    lx::gfx::cpu_compositor comp;
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));
    LUMEN_CHECK(comp.has_texture(1));
    comp.forget_texture(1);
    LUMEN_CHECK(!comp.has_texture(1));

    const auto cmd = quad(1, {0, 0, 2, 2});
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));
    LUMEN_CHECK(stats.value().draws_skipped == 1);
}

/// Row bands must tile the damage rect exactly — no gaps, no rows written twice.
LUMEN_TEST(cpu_composite_multithreaded_bands_cover_every_row) {
    constexpr unsigned k_w = 64;
    constexpr unsigned k_h = 512;

    image src{k_w, k_h, k_xrgb};
    for (unsigned y = 0; y < k_h; ++y)
        for (unsigned x = 0; x < k_w; ++x)
            src.pixels[y * k_w + x] = 0xFF000000u | (y << 8) | x;

    image dst{k_w, k_h, k_xrgb, 0u};

    lx::gfx::cpu_compositor comp;
    comp.set_worker_threads(3);
    LUMEN_CHECK(comp.worker_threads() == 3);
    LUMEN_CHECK(static_cast<bool>(comp.register_texture(1, src.surface())));

    const auto cmd = quad(1, {0, 0, static_cast<int>(k_w), static_cast<int>(k_h)});
    auto stats = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
    LUMEN_CHECK(static_cast<bool>(stats));

    for (unsigned y = 0; y < k_h; ++y) {
        for (unsigned x = 0; x < k_w; ++x)
            LUMEN_CHECK(dst.at(x, y) == src.at(x, y));
    }

    // Reusing the pool across frames must not deadlock or lose bands.
    for (unsigned frame = 0; frame < 8; ++frame) {
        auto again = comp.composite(dst.surface(), lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, {});
        LUMEN_CHECK(static_cast<bool>(again));
    }
    LUMEN_CHECK(dst.at(k_w - 1, k_h - 1) == src.at(k_w - 1, k_h - 1));
}

LUMEN_TEST(cpu_composite_rejects_an_invalid_destination) {
    lx::gfx::cpu_compositor comp;
    lx::gfx::pixel_surface bad{};
    auto stats = comp.composite(bad, lx::color::rgb(0.f, 0.f, 0.f), nullptr, 0, {});
    LUMEN_CHECK(!stats);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
