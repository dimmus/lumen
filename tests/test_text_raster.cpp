// lx.text single-line rasterization: shape_line / fit_line / render_line.
#include "lumen_test.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

import lx.foundation;
import lx.text;

namespace {

constexpr lx::text::font_desc k_font{"Sans", 10.f, lx::text::weight::bold,
                                     lx::text::style::normal};

/// Shapes or skips: without HarfBuzz/FreeType (or with no font installed) every call fails
/// the same way, which the last test checks.
bool shape(const char* text, lx::text::shaped_line& out) {
    return static_cast<bool>(lx::text::shape_line(text, k_font, out));
}

/// Bounding box of every pixel with nonzero alpha.
struct ink {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    unsigned count = 0;
};

ink ink_of(const std::vector<std::uint32_t>& px, int w, int h) {
    ink k{};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (px[static_cast<std::size_t>(y * w + x)] >> 24) {
                ++k.count;
                k.x0 = x < k.x0 ? x : k.x0;
                k.y0 = y < k.y0 ? y : k.y0;
                k.x1 = x > k.x1 ? x : k.x1;
                k.y1 = y > k.y1 ? y : k.y1;
            }
    return k;
}

} // namespace

LUMEN_TEST(render_draws_inside_the_expected_box_only) {
    static lx::text::shaped_line line{};
    if (!shape("Lumen", line))
        return; // no face on this host: covered by the not_implemented/io test
    LUMEN_CHECK(line.count == 5);
    LUMEN_CHECK(line.width_px > 10);
    LUMEN_CHECK(line.ascent_px > 0 && line.descent_px > 0);
    const int w = line.width_px + 20;
    const int h = line.ascent_px + line.descent_px + 10;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w * h), 0u);
    const lx::text::line_fit fit = lx::text::fit_line(line, w);
    LUMEN_CHECK(!fit.ellipsized && fit.glyph_end == line.count);
    const lx::point2i base{10, 5 + line.ascent_px};
    LUMEN_CHECK(static_cast<bool>(lx::text::render_line(
        line, fit, {255, 255, 255, 255}, {px.data(), w, h, w}, base)));
    const ink k = ink_of(px, w, h);
    std::printf("    \"Lumen\" %u glyphs, %d px wide, ink %u px in [%d,%d]-[%d,%d]\n", line.count,
                line.width_px, k.count, k.x0, k.y0, k.x1, k.y1);
    LUMEN_CHECK(k.count > 50);
    // Ink stays within the pen run (a pixel of overhang either side) and the line's
    // vertical metrics.
    LUMEN_CHECK(k.x0 >= base.x - 2 && k.x1 <= base.x + line.width_px + 2);
    LUMEN_CHECK(k.y0 >= base.y - line.ascent_px && k.y1 <= base.y + line.descent_px);
    // Premultiplied: no channel exceeds alpha.
    for (std::uint32_t p : px) {
        const unsigned a = p >> 24;
        LUMEN_CHECK(((p >> 16) & 0xFFu) <= a && ((p >> 8) & 0xFFu) <= a && (p & 0xFFu) <= a);
    }
}

LUMEN_TEST(render_clips_to_the_target) {
    static lx::text::shaped_line line{};
    if (!shape("clipped text", line))
        return;
    std::vector<std::uint32_t> px(8 * 8, 0u);
    // Mostly outside: must not write out of bounds (ASan would catch it) and must succeed.
    LUMEN_CHECK(static_cast<bool>(lx::text::render_line(
        line, lx::text::fit_line(line, 1000), {255, 0, 0, 255}, {px.data(), 8, 8, 8}, {-20, 6})));
}

LUMEN_TEST(fit_never_exceeds_the_width_and_ellipsizes_only_when_needed) {
    static lx::text::shaped_line line{};
    if (!shape("A fairly long window title that will not fit", line))
        return;
    const lx::text::line_fit whole = lx::text::fit_line(line, line.width_px);
    LUMEN_CHECK(!whole.ellipsized && whole.glyph_end == line.count &&
                whole.width_px == line.width_px);
    for (int max = line.width_px - 1; max > 0; max -= 7) {
        const lx::text::line_fit f = lx::text::fit_line(line, max);
        LUMEN_CHECK(f.width_px <= max);
        if (f.width_px > 0) {
            LUMEN_CHECK(f.ellipsized);
            LUMEN_CHECK(f.glyph_end < line.count);
        }
    }
    // Monotonic: more room never keeps fewer glyphs.
    unsigned prev = 0;
    for (int max = 1; max < line.width_px; ++max) {
        const lx::text::line_fit f = lx::text::fit_line(line, max);
        LUMEN_CHECK(f.glyph_end >= prev);
        prev = f.glyph_end;
    }
}

LUMEN_TEST(fit_cuts_on_cluster_boundaries) {
    static lx::text::shaped_line line{};
    // "e" + combining acute, repeated: each pair is one cluster of up to two glyphs.
    if (!shape("e\xCC\x81"
               "e\xCC\x81"
               "e\xCC\x81"
               "e\xCC\x81"
               "e\xCC\x81"
               "e\xCC\x81"
               "e\xCC\x81"
               "e\xCC\x81",
               line))
        return;
    for (int max = 1; max < line.width_px; ++max) {
        const lx::text::line_fit f = lx::text::fit_line(line, max);
        if (f.glyph_end == 0 || f.glyph_end >= line.count)
            continue;
        LUMEN_CHECK(line.cluster[f.glyph_end] != line.cluster[f.glyph_end - 1]);
    }
}

LUMEN_TEST(empty_and_degenerate_inputs) {
    static lx::text::shaped_line line{};
    const auto r = lx::text::shape_line("", k_font, line);
    if (!r)
        return;
    LUMEN_CHECK(line.count == 0 && line.width_px == 0);
    const lx::text::line_fit f = lx::text::fit_line(line, 100);
    LUMEN_CHECK(f.glyph_end == 0 && f.width_px == 0 && !f.ellipsized);
    std::vector<std::uint32_t> px(4, 0u);
    LUMEN_CHECK(static_cast<bool>(
        lx::text::render_line(line, f, {255, 255, 255, 255}, {px.data(), 2, 2, 2}, {0, 1})));
    // A null target is an argument error, never a crash.
    LUMEN_CHECK(!lx::text::render_line(line, f, {}, {nullptr, 2, 2, 2}, {0, 0}));
    // Zero width keeps nothing.
    static lx::text::shaped_line abc{};
    if (shape("abc", abc))
        LUMEN_CHECK(lx::text::fit_line(abc, 0).glyph_end == 0);
}

LUMEN_TEST(failure_is_reported_not_faked) {
    static lx::text::shaped_line line{};
    const auto r = lx::text::shape_line("x", k_font, line);
    if (r)
        return; // fonts present: the other cases ran
    const auto d = r.get_error().domain;
    LUMEN_CHECK(d == lx::error_domain::not_implemented || d == lx::error_domain::io);
    LUMEN_CHECK(line.count == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
