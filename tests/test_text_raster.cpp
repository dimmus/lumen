// lx.text single-line rasterization: shape_line / fit_line / render_line; and the glyph
// atlas raster (`lx.text:raster`, ADR-010 §4.6): subpixel variants, cache hits, eviction and
// rebuild, colour glyphs on an RGBA8 page.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.text;

#ifndef LUMEN_TEST_FONT_DIR
#define LUMEN_TEST_FONT_DIR "tests/fonts"
#endif

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

// ── Glyph atlas raster (lx.text:raster) ──

namespace {

/// A face cache over the bundled fonts plus tests/fonts, its shape cache and an atlas with a
/// glyph cache on it. Heap-allocated: the atlas and the caches are large.
struct raster_fixture {
    std::unique_ptr<lx::text::face_cache> faces = std::make_unique<lx::text::face_cache>();
    std::unique_ptr<lx::text::shape_cache> shapes = std::make_unique<lx::text::shape_cache>();
    std::unique_ptr<lx::gfx::atlas> atlas = std::make_unique<lx::gfx::atlas>();
    std::unique_ptr<lx::text::glyph_cache> glyphs;
    explicit raster_fixture(lx::text::glyph_cache_config config = {})
        : glyphs{std::make_unique<lx::text::glyph_cache>(*atlas, config)} {
        (void)faces->add_font_dir(LUMEN_TEST_FONT_DIR);
    }

    /// The glyph ids of `utf8` in `face` at `size` (26.6), visual order; empty without fonts.
    std::vector<std::uint32_t> glyph_ids(lx::text::face_id face, std::int32_t size,
                                         std::string_view utf8) {
        lx::text::shape_request req{};
        req.face = face;
        req.size_26_6 = size;
        req.utf8 = utf8;
        std::vector<std::uint32_t> out;
        if (auto run = shapes->shape(*faces, req))
            for (const lx::text::shaped_glyph& g : run->glyphs)
                out.push_back(g.glyph);
        return out;
    }

    /// The tile's texels, row by row (bytes per texel of its page).
    std::vector<std::uint8_t> texels(const lx::gfx::atlas_tile& t) const {
        const lx::gfx::atlas_page_view v = atlas->page(t.page);
        const unsigned bpt = v.format == lx::gfx::atlas_format::r8 ? 1u : 4u;
        std::vector<std::uint8_t> out;
        for (unsigned y = 0; y < t.h; ++y) {
            const std::uint8_t* row = v.pixels + static_cast<std::size_t>(t.y + y) * v.stride +
                                      static_cast<std::size_t>(t.x) * bpt;
            out.insert(out.end(), row, row + static_cast<std::size_t>(t.w) * bpt);
        }
        return out;
    }
};

constexpr std::int32_t k_14px = 14 * 64;

} // namespace

LUMEN_TEST(glyph_pen_rounds_to_quarters_and_whole_rows) {
    using lx::text::glyph_pen;
    static_assert(glyph_pen(0, 0).x == 0 && glyph_pen(0, 0).variant == 0);
    // 26.6: 16 units are a quarter pixel; the nearest quarter wins.
    static_assert(glyph_pen(7, 0).variant == 0 && glyph_pen(8, 0).variant == 1);
    static_assert(glyph_pen(32, 0).variant == 2 && glyph_pen(48, 0).variant == 3);
    static_assert(glyph_pen(56, 0).x == 1 && glyph_pen(56, 0).variant == 0);
    static_assert(glyph_pen(64 * 5 + 36, 0).x == 5 && glyph_pen(64 * 5 + 36, 0).variant == 2);
    // Negative pens floor: −¼ px is pixel −1, variant 3.
    static_assert(glyph_pen(-16, 0).x == -1 && glyph_pen(-16, 0).variant == 3);
    // y snaps to the nearest whole pixel.
    static_assert(glyph_pen(0, 31).y == 0 && glyph_pen(0, 32).y == 1 && glyph_pen(0, -33).y == -1);
    static_assert(lx::text::effective_hinting(lx::text::glyph_hinting::light, 1.25f) ==
                  lx::text::glyph_hinting::light);
    static_assert(lx::text::effective_hinting(lx::text::glyph_hinting::light, 1.5f) ==
                  lx::text::glyph_hinting::none);
    static_assert(lx::text::effective_hinting(lx::text::glyph_hinting::none, 1.f) ==
                  lx::text::glyph_hinting::none);
}

LUMEN_TEST(subpixel_variants_differ_and_are_cached_separately) {
    raster_fixture fx{};
    const lx::text::face_id face = fx.faces->resolve(lx::text::font_desc{});
    if (!face.valid())
        return; // no faces on this host
    const std::vector<std::uint32_t> ids = fx.glyph_ids(face, k_14px, "l");
    LUMEN_CHECK(ids.size() == 1);
    lx::gfx::atlas_tile tiles[lx::text::k_subpixel_variants]{};
    lx::gfx::atlas_tile_id tile_ids[lx::text::k_subpixel_variants]{};
    std::vector<std::uint8_t> px[lx::text::k_subpixel_variants];
    int lefts[lx::text::k_subpixel_variants]{};
    for (std::uint8_t v = 0; v < lx::text::k_subpixel_variants; ++v) {
        auto e = fx.glyphs->get(*fx.faces, {face, k_14px, ids[0], v});
        LUMEN_CHECK(static_cast<bool>(e));
        LUMEN_CHECK(!(*e)->empty() && !(*e)->color);
        LUMEN_CHECK(fx.glyphs->place(**e, tiles[v]));
        LUMEN_CHECK(tiles[v].page < lx::gfx::k_atlas_first_rgba8_page);
        tile_ids[v] = (*e)->tile;
        lefts[v] = (*e)->left;
        px[v] = fx.texels(tiles[v]);
    }
    for (unsigned a = 0; a < lx::text::k_subpixel_variants; ++a)
        for (unsigned b = a + 1; b < lx::text::k_subpixel_variants; ++b) {
            LUMEN_CHECK(tile_ids[a] != tile_ids[b]);
            // Moved by a quarter pixel: different coverage (the stem straddles columns).
            LUMEN_CHECK(px[a] != px[b] || lefts[a] != lefts[b]);
        }
    const lx::text::glyph_cache_counters c = fx.glyphs->counters();
    std::printf("    'l' 14 px: variants left %d %d %d %d, widths %u %u %u %u\n", lefts[0], lefts[1],
                lefts[2], lefts[3], tiles[0].w, tiles[1].w, tiles[2].w, tiles[3].w);
    LUMEN_CHECK(c.rasterized == 4 && c.live == 4);
    // The ink moves right with the variant: its coverage-weighted centre grows.
    float centre[lx::text::k_subpixel_variants]{};
    for (unsigned v = 0; v < lx::text::k_subpixel_variants; ++v) {
        double sum = 0.0, weighted = 0.0;
        for (unsigned y = 0; y < tiles[v].h; ++y)
            for (unsigned x = 0; x < tiles[v].w; ++x) {
                const double cov = px[v][y * tiles[v].w + x];
                sum += cov;
                weighted += cov * (lefts[v] + static_cast<int>(x) + 0.5);
            }
        centre[v] = static_cast<float>(weighted / sum);
    }
    for (unsigned v = 1; v < lx::text::k_subpixel_variants; ++v) {
        const float step = centre[v] - centre[v - 1];
        LUMEN_CHECK(step > 0.15f && step < 0.35f);
    }
}

LUMEN_TEST(glyph_cache_hit_returns_the_same_tile) {
    raster_fixture fx{};
    const lx::text::face_id face = fx.faces->resolve(lx::text::font_desc{});
    if (!face.valid())
        return;
    const std::vector<std::uint32_t> ids = fx.glyph_ids(face, k_14px, "Lumen");
    LUMEN_CHECK(ids.size() == 5);
    std::vector<lx::gfx::atlas_tile_id> first;
    for (std::uint32_t id : ids) {
        auto e = fx.glyphs->get(*fx.faces, {face, k_14px, id, 1});
        LUMEN_CHECK(static_cast<bool>(e));
        first.push_back((*e)->tile);
    }
    const lx::text::glyph_cache_counters before = fx.glyphs->counters();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const lx::text::glyph_entry* hit = fx.glyphs->find({face, k_14px, ids[i], 1});
        LUMEN_CHECK(hit && hit->tile == first[i]);
    }
    const lx::text::glyph_cache_counters after = fx.glyphs->counters();
    LUMEN_CHECK(after.rasterized == before.rasterized);
    LUMEN_CHECK(after.hits == before.hits + ids.size());
    // Another size, hinting or face is another glyph.
    LUMEN_CHECK(fx.glyphs->find({face, k_14px + 64, ids[0], 1}) == nullptr);
    LUMEN_CHECK(fx.glyphs->find({face, k_14px, ids[0], 1, lx::text::glyph_hinting::light}) ==
                nullptr);
    auto hinted = fx.glyphs->get(*fx.faces, {face, k_14px, ids[0], 1, lx::text::glyph_hinting::light});
    LUMEN_CHECK(static_cast<bool>(hinted) && (*hinted)->tile != first[0]);
    // A space has no ink: cached, but no tile and nothing to place.
    const std::vector<std::uint32_t> space = fx.glyph_ids(face, k_14px, " ");
    LUMEN_CHECK(space.size() == 1);
    auto blank = fx.glyphs->get(*fx.faces, {face, k_14px, space[0], 0});
    LUMEN_CHECK(static_cast<bool>(blank) && (*blank)->empty());
    lx::gfx::atlas_tile none{};
    LUMEN_CHECK(!fx.glyphs->place(**blank, none));
    LUMEN_CHECK(fx.glyphs->find({face, k_14px, space[0], 0}) != nullptr);
    // A stale face is an io error, never a crash.
    auto stale = fx.glyphs->get(*fx.faces, {lx::text::face_id{0xFFFFFFu}, k_14px, ids[0], 0});
    LUMEN_CHECK(!stale && stale.get_error().domain == lx::error_domain::io);
}

LUMEN_TEST(eviction_and_rebuild_keep_glyphs_drawable) {
    raster_fixture fx{};
    const lx::text::face_id face = fx.faces->resolve(lx::text::font_desc{});
    if (!face.valid())
        return;
    const std::vector<std::uint32_t> ids = fx.glyph_ids(face, k_14px, "abcdefgh");
    LUMEN_CHECK(ids.size() == 8);
    fx.atlas->begin_frame(1);
    std::vector<std::vector<std::uint8_t>> original;
    for (std::uint32_t id : ids) {
        auto e = fx.glyphs->get(*fx.faces, {face, k_14px, id, 0});
        LUMEN_CHECK(static_cast<bool>(e));
        lx::gfx::atlas_tile t{};
        LUMEN_CHECK(fx.glyphs->place(**e, t));
        original.push_back(fx.texels(t));
    }
    // Keep "abcd" in use, let "efgh" go stale, then rebuild their page: the stale tiles are
    // dropped, the live ones move — their ids stay, `place` finds the new texels.
    const unsigned page = [&] {
        lx::gfx::atlas_tile t{};
        (void)fx.glyphs->place(*fx.glyphs->find({face, k_14px, ids[0], 0}), t);
        return static_cast<unsigned>(t.page);
    }();
    for (std::uint64_t frame = 2; frame <= 200; ++frame) {
        fx.atlas->begin_frame(frame);
        for (unsigned i = 0; i < 4; ++i) {
            const lx::text::glyph_entry* e = fx.glyphs->find({face, k_14px, ids[i], 0});
            lx::gfx::atlas_tile t{};
            LUMEN_CHECK(e && fx.glyphs->place(*e, t));
        }
    }
    auto rebuilt = fx.atlas->rebuild(page);
    LUMEN_CHECK(static_cast<bool>(rebuilt));
    std::printf("    rebuild: %u evicted, %u moved\n", rebuilt->evicted, rebuilt->moved);
    LUMEN_CHECK(rebuilt->evicted == 4 && rebuilt->lost == 0);
    const lx::text::glyph_cache_counters before = fx.glyphs->counters();
    for (unsigned i = 0; i < 8; ++i) {
        const lx::text::glyph_key key{face, k_14px, ids[i], 0};
        const lx::text::glyph_entry* e = fx.glyphs->find(key);
        if (i < 4) {
            // Live: found where the rebuild put it, with the same pixels.
            lx::gfx::atlas_tile t{};
            LUMEN_CHECK(e && fx.glyphs->place(*e, t));
            LUMEN_CHECK(fx.texels(t) == original[i]);
        } else {
            // Evicted: a miss, rasterised again on demand, identical.
            LUMEN_CHECK(e == nullptr);
            auto again = fx.glyphs->get(*fx.faces, key);
            LUMEN_CHECK(static_cast<bool>(again));
            lx::gfx::atlas_tile t{};
            LUMEN_CHECK(fx.glyphs->place(**again, t));
            LUMEN_CHECK(fx.texels(t) == original[i]);
        }
    }
    const lx::text::glyph_cache_counters after = fx.glyphs->counters();
    LUMEN_CHECK(after.rerasterized == before.rerasterized + 4);
    LUMEN_CHECK(after.rasterized == before.rasterized + 4);
    LUMEN_CHECK(after.live == before.live);

    // In-place eviction (no rebuild) behaves the same way.
    for (std::uint64_t frame = 201; frame <= 330; ++frame)
        fx.atlas->begin_frame(frame);
    LUMEN_CHECK(fx.atlas->evict_stale() == 8);
    for (std::uint32_t id : ids) {
        LUMEN_CHECK(fx.glyphs->find({face, k_14px, id, 0}) == nullptr);
        auto e = fx.glyphs->get(*fx.faces, {face, k_14px, id, 0});
        lx::gfx::atlas_tile t{};
        LUMEN_CHECK(e && fx.glyphs->place(**e, t));
    }
}

LUMEN_TEST(full_sets_replace_their_least_recently_used_way) {
    // One set of 8 ways: the ninth glyph replaces the oldest; the frame's tiles stay.
    lx::text::glyph_cache_config one_set{};
    one_set.entries = lx::text::k_glyph_cache_ways;
    raster_fixture fx{one_set};
    const lx::text::face_id face = fx.faces->resolve(lx::text::font_desc{});
    if (!face.valid())
        return;
    const std::vector<std::uint32_t> ids = fx.glyph_ids(face, k_14px, "abcdefghij");
    LUMEN_CHECK(ids.size() == 10);
    fx.atlas->begin_frame(1);
    for (unsigned i = 0; i < 8; ++i)
        LUMEN_CHECK(static_cast<bool>(fx.glyphs->get(*fx.faces, {face, k_14px, ids[i], 0})));
    fx.atlas->begin_frame(2);
    // Touch "b" … "h": "a" is the least recently used.
    for (unsigned i = 1; i < 8; ++i)
        LUMEN_CHECK(fx.glyphs->find({face, k_14px, ids[i], 0}) != nullptr);
    const unsigned tiles_before = fx.atlas->stats().live_tiles;
    LUMEN_CHECK(static_cast<bool>(fx.glyphs->get(*fx.faces, {face, k_14px, ids[8], 0})));
    LUMEN_CHECK(fx.glyphs->find({face, k_14px, ids[0], 0}) == nullptr);
    const lx::text::glyph_cache_counters c = fx.glyphs->counters();
    LUMEN_CHECK(c.evictions == 1 && c.tiles_released == 1 && c.live == 8 && c.capacity == 8);
    // "a" was last used in frame 1, so its tile went back to the atlas.
    LUMEN_CHECK(fx.atlas->stats().live_tiles == tiles_before);
    // `reject` keeps the set and fails the glyph.
    lx::text::glyph_cache_config strict = one_set;
    strict.overflow = lx::runtime::overflow_action::reject;
    lx::text::glyph_cache rejecting{*fx.atlas, strict};
    for (unsigned i = 0; i < 8; ++i)
        LUMEN_CHECK(static_cast<bool>(rejecting.get(*fx.faces, {face, k_14px, ids[i], 0})));
    auto refused = rejecting.get(*fx.faces, {face, k_14px, ids[9], 0});
    LUMEN_CHECK(!refused && rejecting.counters().overflows == 1);
}

LUMEN_TEST(colour_glyph_lands_on_an_rgba8_page) {
    raster_fixture fx{};
    const lx::text::face_id face = fx.faces->resolve("Lumen Test Emoji", lx::text::weight::normal,
                                                     lx::text::style::normal);
    lx::text::face_info info{};
    if (!face.valid() || !fx.faces->describe(face, info) ||
        std::strstr(info.file, "LumenTestEmoji") == nullptr) {
        std::printf("    (skip: tests/fonts not registered)\n");
        return;
    }
    // 😀 U+1F600: COLRv0 layers.
    const std::vector<std::uint32_t> ids = fx.glyph_ids(face, 24 * 64, "\xF0\x9F\x98\x80");
    LUMEN_CHECK(ids.size() == 1 && ids[0] != 0);
    auto e = fx.glyphs->get(*fx.faces, {face, 24 * 64, ids[0], 0});
    LUMEN_CHECK(static_cast<bool>(e));
    LUMEN_CHECK((*e)->color && !(*e)->empty());
    lx::gfx::atlas_tile t{};
    LUMEN_CHECK(fx.glyphs->place(**e, t));
    LUMEN_CHECK(t.page >= lx::gfx::k_atlas_first_rgba8_page);
    LUMEN_CHECK(lx::gfx::atlas::page_format(t.page) == lx::gfx::atlas_format::rgba8);
    std::printf("    U+1F600 at 24 px: %u x %u, bearing (%d, %d), page %u\n", t.w, t.h, (*e)->left,
                (*e)->top, t.page);
    LUMEN_CHECK(t.w >= 18 && t.w <= 30 && t.h >= 18 && t.h <= 30);
    // Premultiplied and coloured: no channel above alpha, and the face is yellow (R, G > B).
    const std::vector<std::uint8_t> px = fx.texels(t);
    unsigned long long r = 0, g = 0, b = 0, opaque = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        LUMEN_CHECK(px[i] <= px[i + 3] && px[i + 1] <= px[i + 3] && px[i + 2] <= px[i + 3]);
        r += px[i];
        g += px[i + 1];
        b += px[i + 2];
        opaque += px[i + 3] == 255 ? 1u : 0u;
    }
    LUMEN_CHECK(opaque > px.size() / 4 / 3);
    LUMEN_CHECK(r > 2 * b && g > 2 * b);
    // Colour glyphs have no subpixel variants: every variant answers with the same tile.
    for (std::uint8_t v = 1; v < lx::text::k_subpixel_variants; ++v) {
        auto ev = fx.glyphs->get(*fx.faces, {face, 24 * 64, ids[0], v});
        LUMEN_CHECK(static_cast<bool>(ev) && (*ev)->tile == (*e)->tile);
    }
    LUMEN_CHECK(fx.glyphs->counters().color == 1 && fx.glyphs->counters().rasterized == 1);
}

int main(int argc, char** argv) {
    // The atlas the glyph cache writes is UI affinity.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
