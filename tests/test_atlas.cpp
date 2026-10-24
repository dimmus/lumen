#include "lumen_test.hpp"

#include <sys/mman.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.gfx;

// ADR-010 Phase 1: `lx.gfx:atlas` — shelf packing, the 120-frame eviction rule, page
// rebuilds, dirty-rect coalescing and the fixed caps.

namespace {

using lx::gfx::atlas;
using lx::gfx::atlas_format;
using lx::gfx::atlas_tile;
using lx::gfx::atlas_tile_id;

struct test_rng {
    std::uint64_t state = 0x2545F4914F6CDD1Dull;
    [[nodiscard]] std::uint64_t next() {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return state;
    }
    [[nodiscard]] float unit() { return static_cast<float>(next() % 1000000u) / 1000000.f; }
};

/// A glyph box as FreeType hands it for UI text: sizes 10–20 px weighted to the 14 px UI
/// size (ADR-010 §5.4), heights from x-height to ascender + descender, widths from an
/// "i" to an "m", plus one texel of antialiasing on each axis.
void glyph_box(test_rng& rng, unsigned& w, unsigned& h) {
    static constexpr float k_sizes[] = {10.f, 12.f, 12.f, 14.f, 14.f, 14.f, 14.f, 16.f, 16.f, 20.f};
    const float size = k_sizes[rng.next() % 10u];
    const float hf = size * (0.50f + 0.55f * rng.unit());
    const float wf = size * (0.15f + 0.65f * rng.unit());
    h = static_cast<unsigned>(hf + 0.5f) + 1u;
    w = static_cast<unsigned>(wf + 0.5f) + 1u;
}

/// A texel value derived from the tile and the texel, so moved tiles can be recognised.
[[nodiscard]] std::uint8_t pattern(unsigned seed, unsigned x, unsigned y) {
    std::uint32_t v = seed * 2654435761u ^ (x * 40503u) ^ (y * 9973u);
    v ^= v >> 13u;
    return static_cast<std::uint8_t>(v * 0x9E37u >> 8u);
}

void write_pattern(atlas& a, atlas_tile_id id, unsigned seed) {
    atlas_tile t{};
    LUMEN_CHECK(a.lookup(id, t));
    const unsigned bpt = atlas::page_format(t.page) == atlas_format::r8 ? 1u : 4u;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(t.w) * t.h * bpt);
    for (unsigned y = 0; y < t.h; ++y)
        for (unsigned x = 0; x < t.w * bpt; ++x)
            px[static_cast<std::size_t>(y) * t.w * bpt + x] = pattern(seed, x, y);
    LUMEN_CHECK(static_cast<bool>(a.write(id, px, t.w * bpt)));
}

[[nodiscard]] bool pattern_intact(const atlas& a, atlas_tile_id id, unsigned seed) {
    atlas_tile t{};
    if (!a.lookup(id, t))
        return false;
    const lx::gfx::atlas_page_view page = a.page(t.page);
    const unsigned bpt = page.format == atlas_format::r8 ? 1u : 4u;
    for (unsigned y = 0; y < t.h; ++y)
        for (unsigned x = 0; x < t.w * bpt; ++x)
            if (page.pixels[static_cast<std::size_t>(t.y + y) * page.stride + t.x * bpt + x] !=
                pattern(seed, x, y))
                return false;
    return true;
}

/// Every live tile of `ids` inside its page and no texel claimed twice.
[[nodiscard]] bool no_overlap(const atlas& a, std::span<const atlas_tile_id> ids) {
    std::vector<std::uint8_t> claimed(static_cast<std::size_t>(lx::gfx::k_atlas_pages) *
                                      lx::gfx::k_atlas_page_max * lx::gfx::k_atlas_page_max);
    for (const atlas_tile_id id : ids) {
        atlas_tile t{};
        if (!a.lookup(id, t))
            continue;
        const unsigned size = a.page(t.page).size;
        if (t.x + t.w > size || t.y + t.h > size)
            return false;
        for (unsigned y = t.y; y < t.y + t.h; ++y) {
            std::uint8_t* row = claimed.data() +
                                (static_cast<std::size_t>(t.page) * lx::gfx::k_atlas_page_max + y) *
                                    lx::gfx::k_atlas_page_max;
            for (unsigned x = t.x; x < t.x + t.w; ++x) {
                if (row[x])
                    return false;
                row[x] = 1;
            }
        }
    }
    return true;
}

} // namespace

LUMEN_TEST(bucket_heights_round_up) {
    LUMEN_CHECK(atlas::bucket_height(1) == 2 && atlas::bucket_height(2) == 2);
    LUMEN_CHECK(atlas::bucket_height(13) == 14 && atlas::bucket_height(32) == 32);
    LUMEN_CHECK(atlas::bucket_height(33) == 36 && atlas::bucket_height(65) == 72);
    LUMEN_CHECK(atlas::bucket_height(129) == 144);
    LUMEN_CHECK(atlas::page_format(3) == atlas_format::r8);
    LUMEN_CHECK(atlas::page_format(4) == atlas_format::rgba8);
}

// Fill R8 page 0 with UI glyph boxes until it is full at 2048² (the next tile opens page
// 1), then check every tile exhaustively for overlap and report the density.
LUMEN_TEST(glyph_packing_density_and_no_overlap) {
    auto a = std::make_unique<atlas>();
    test_rng rng{};
    std::vector<atlas_tile_id> ids;
    ids.reserve(lx::gfx::k_atlas_max_tiles);
    float density_at_1024 = 0.f;
    for (;;) {
        unsigned w = 0;
        unsigned h = 0;
        glyph_box(rng, w, h);
        auto id = a->allocate(atlas_format::r8, w, h);
        LUMEN_CHECK(static_cast<bool>(id));
        atlas_tile t{};
        LUMEN_CHECK(a->lookup(*id, t));
        if (t.page != 0)
            break; // page 0 is full
        if (a->page(0).size == lx::gfx::k_atlas_page_initial)
            density_at_1024 = a->density(0);
        ids.push_back(*id);
        if (ids.size() + 1 >= lx::gfx::k_atlas_max_tiles)
            break;
    }
    const float density = a->density(0);
    std::printf("    glyph packing: %zu tiles, page 0 %u², density %.1f %% (%.1f %% when full at "
                "1024²), shelves claim %.1f %%\n",
                ids.size(), a->page(0).size, static_cast<double>(density) * 100.0,
                static_cast<double>(density_at_1024) * 100.0,
                static_cast<double>(a->occupancy(0)) * 100.0);
    LUMEN_CHECK(no_overlap(*a, ids));
    // The tile table caps a page of 14 px glyphs before the page fills; whichever stops the
    // fill, the packed area must be dense.
    LUMEN_CHECK(density_at_1024 > 0.80f);
    LUMEN_CHECK(a->density(0) > 0.80f || ids.size() + 1 >= lx::gfx::k_atlas_max_tiles);
}

LUMEN_TEST(random_alloc_release_never_overlaps) {
    auto a = std::make_unique<atlas>();
    test_rng rng{};
    std::vector<atlas_tile_id> live;
    // One frame throughout: nothing goes stale, so nothing but `release` frees a tile.
    for (unsigned step = 0; step < 20000; ++step) {
        if (!live.empty() && rng.next() % 3u == 0) {
            const std::size_t k = rng.next() % live.size();
            a->release(live[k]);
            live[k] = live.back();
            live.pop_back();
            continue;
        }
        const unsigned w = 1u + static_cast<unsigned>(rng.next() % 40u);
        const unsigned h = 1u + static_cast<unsigned>(rng.next() % 40u);
        auto id = a->allocate(atlas_format::r8, w, h);
        LUMEN_CHECK(static_cast<bool>(id));
        live.push_back(*id);
    }
    LUMEN_CHECK(no_overlap(*a, live));
    unsigned total = 0;
    for (unsigned p = 0; p < lx::gfx::k_atlas_pages; ++p)
        total += a->live_tiles(p);
    LUMEN_CHECK(total == live.size());
}

LUMEN_TEST(eviction_respects_the_120_frame_rule) {
    atlas a{};
    a.begin_frame(100);
    auto old_tile = a.allocate(atlas_format::r8, 10, 12);
    auto kept = a.allocate(atlas_format::r8, 10, 12);
    LUMEN_CHECK(static_cast<bool>(old_tile) && static_cast<bool>(kept));
    for (std::uint64_t f = 101; f <= 219; ++f) {
        a.begin_frame(f);
        a.touch(*kept);
    }
    // Last used at 100: at 219 it was used 119 frames ago — inside the window.
    LUMEN_CHECK(a.evict_stale() == 0);
    atlas_tile t{};
    LUMEN_CHECK(a.lookup(*old_tile, t));
    a.begin_frame(220);
    LUMEN_CHECK(a.stale_tiles(0) == 1);
    LUMEN_CHECK(a.evict_stale() == 1);
    LUMEN_CHECK(!a.lookup(*old_tile, t));
    LUMEN_CHECK(a.lookup(*kept, t));
    LUMEN_CHECK(a.stats().evicted == 1);
    // A stale id cannot be written or touched back to life.
    const std::uint8_t px[120]{};
    LUMEN_CHECK(!a.write(*old_tile, px, 10));
    a.touch(*old_tile);
    LUMEN_CHECK(!a.lookup(*old_tile, t));
}

LUMEN_TEST(rebuild_preserves_live_tile_contents) {
    auto a = std::make_unique<atlas>();
    a->begin_frame(1);
    test_rng rng{};
    std::vector<atlas_tile_id> ids;
    std::vector<unsigned> seeds;
    for (unsigned i = 0; i < 3000; ++i) {
        unsigned w = 0;
        unsigned h = 0;
        glyph_box(rng, w, h);
        auto id = a->allocate(atlas_format::r8, w, h);
        LUMEN_CHECK(static_cast<bool>(id));
        write_pattern(*a, *id, i);
        ids.push_back(*id);
        seeds.push_back(i);
    }
    a->clear_dirty(0);
    // Two thirds stay in use; the rest go stale.
    for (std::uint64_t f = 2; f <= 200; ++f) {
        a->begin_frame(f);
        for (unsigned i = 0; i < ids.size(); ++i)
            if (i % 3u != 0)
                a->touch(ids[i]);
    }
    const float before = a->occupancy(0);
    auto r = a->rebuild(0);
    LUMEN_CHECK(static_cast<bool>(r));
    std::printf("    rebuild: %u evicted, %u moved, occupancy %.1f %% -> %.1f %%, %.1f us\n",
                r->evicted, r->moved, static_cast<double>(before) * 100.0,
                static_cast<double>(r->occupancy_after) * 100.0,
                static_cast<double>(r->ns) / 1000.0);
    LUMEN_CHECK(r->evicted == 1000 && r->moved == 2000 && r->lost == 0);
    LUMEN_CHECK(r->occupancy_after < before);
    LUMEN_CHECK(a->dirty_full(0));
    for (unsigned i = 0; i < ids.size(); ++i) {
        atlas_tile t{};
        if (i % 3u == 0)
            LUMEN_CHECK(!a->lookup(ids[i], t));
        else
            LUMEN_CHECK(pattern_intact(*a, ids[i], seeds[i]));
    }
    LUMEN_CHECK(no_overlap(*a, ids));
    LUMEN_CHECK(a->stats().rebuilds == 1);
}

// Every R8 page full of 64 × 64 tiles (shelves claim 100 %): the next allocation reclaims —
// stale tiles freed in place, pages above 85 % marked for a rebuild — and succeeds; no live
// tile moves inside `allocate` (a uv looked up earlier in the frame still names its pixels);
// the rebuild runs at the next `begin_frame`; recent tiles survive both.
LUMEN_TEST(full_atlas_rebuilds_pages_with_stale_tiles) {
    auto a = std::make_unique<atlas>();
    a->begin_frame(10);
    std::vector<atlas_tile_id> ids;
    for (;;) {
        auto id = a->allocate(atlas_format::r8, 64, 64);
        if (!id)
            break;
        write_pattern(*a, *id, static_cast<unsigned>(ids.size()));
        ids.push_back(*id);
    }
    const std::size_t per_page = (lx::gfx::k_atlas_page_max / 64u) * (lx::gfx::k_atlas_page_max / 64u);
    LUMEN_CHECK(ids.size() == per_page * lx::gfx::k_atlas_r8_pages);
    LUMEN_CHECK(a->occupancy(0) > 0.99f);
    // Nothing is stale yet: a full atlas of live tiles refuses.
    LUMEN_CHECK(!a->allocate(atlas_format::r8, 64, 64));
    a->begin_frame(200);
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (i % 2u == 0)
            a->touch(ids[i]);
    // The frame's sprites looked their uvs up before the allocation.
    std::vector<atlas_tile> before(ids.size());
    for (std::size_t i = 0; i < ids.size(); i += 2)
        LUMEN_CHECK(a->lookup(ids[i], before[i]));
    auto fresh = a->allocate(atlas_format::r8, 64, 64);
    LUMEN_CHECK(static_cast<bool>(fresh));
    LUMEN_CHECK(a->stats().rebuilds == 0 && a->stats().evicted > 0);
    bool pending = false;
    for (unsigned p = 0; p < lx::gfx::k_atlas_r8_pages; ++p)
        pending = pending || a->rebuild_pending(p);
    LUMEN_CHECK(pending);
    for (std::size_t i = 0; i < ids.size(); i += 2) {
        atlas_tile now{};
        LUMEN_CHECK(a->lookup(ids[i], now));
        LUMEN_CHECK(now.page == before[i].page && now.x == before[i].x && now.y == before[i].y);
        LUMEN_CHECK(pattern_intact(*a, ids[i], static_cast<unsigned>(i)));
    }
    a->begin_frame(201);
    for (unsigned p = 0; p < lx::gfx::k_atlas_r8_pages; ++p)
        LUMEN_CHECK(!a->rebuild_pending(p));
    const lx::gfx::atlas_stats st = a->stats();
    std::printf("    reclaim: %llu rebuilds, %llu evicted, last rebuild %.1f us, max %.1f us\n",
                st.rebuilds, st.evicted, static_cast<double>(st.last_rebuild_ns) / 1000.0,
                static_cast<double>(st.max_rebuild_ns) / 1000.0);
    LUMEN_CHECK(st.rebuilds >= 1);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        atlas_tile t{};
        if (i % 2u == 0)
            LUMEN_CHECK(pattern_intact(*a, ids[i], static_cast<unsigned>(i)));
    }
    ids.push_back(*fresh);
    LUMEN_CHECK(no_overlap(*a, ids));
}

LUMEN_TEST(pages_grow_once_and_keep_tiles) {
    auto a = std::make_unique<atlas>();
    auto first = a->allocate(atlas_format::r8, 1000, 600);
    LUMEN_CHECK(static_cast<bool>(first));
    write_pattern(*a, *first, 7);
    LUMEN_CHECK(a->page(0).size == lx::gfx::k_atlas_page_initial);
    LUMEN_CHECK(!a->dirty_full(0) && a->dirty_rects(0).size() == 1);
    // 600 + 600 rows do not fit 1024: the page grows (whole-page dirty), the tile stays.
    auto second = a->allocate(atlas_format::r8, 1000, 600);
    LUMEN_CHECK(static_cast<bool>(second));
    atlas_tile t{};
    LUMEN_CHECK(a->lookup(*second, t) && t.page == 0);
    LUMEN_CHECK(a->page(0).size == lx::gfx::k_atlas_page_max);
    LUMEN_CHECK(a->dirty_full(0) && a->dirty_rects(0).empty());
    LUMEN_CHECK(pattern_intact(*a, *first, 7));
    LUMEN_CHECK(a->stats().grows == 1);
}

LUMEN_TEST(dirty_rects_coalesce) {
    atlas a{};
    std::vector<atlas_tile_id> row;
    for (unsigned i = 0; i < 3; ++i) {
        auto id = a.allocate(atlas_format::r8, 8, 8);
        LUMEN_CHECK(static_cast<bool>(id));
        row.push_back(*id);
    }
    // Tiles 0 and 2 are 8 px apart: two rects. Tile 1 fills the gap: one strip.
    write_pattern(a, row[0], 0);
    write_pattern(a, row[2], 2);
    LUMEN_CHECK(a.dirty_rects(0).size() == 2);
    write_pattern(a, row[1], 1);
    LUMEN_CHECK(a.dirty_rects(0).size() == 1);
    const lx::rect2i strip = a.dirty_rects(0)[0];
    LUMEN_CHECK(strip.x == 0 && strip.y == 0 && strip.width == 24 && strip.height == 8);
    // Rewriting inside a dirty rect adds nothing.
    write_pattern(a, row[1], 1);
    LUMEN_CHECK(a.dirty_rects(0).size() == 1);
    a.clear_dirty(0);
    LUMEN_CHECK(a.dirty_rects(0).empty() && !a.dirty_full(0));
}

LUMEN_TEST(dirty_list_overflow_marks_the_page) {
    auto a = std::make_unique<atlas>();
    // 8 × 8 tiles fill shelves of 128; writing every other tile of every other shelf
    // leaves no two written tiles mergeable.
    std::vector<atlas_tile_id> ids;
    for (unsigned i = 0; i < 128u * 9u; ++i) {
        auto id = a->allocate(atlas_format::r8, 8, 8);
        LUMEN_CHECK(static_cast<bool>(id));
        ids.push_back(*id);
    }
    unsigned written = 0;
    for (unsigned i = 0; i < ids.size(); ++i) {
        const unsigned col = i % 128u;
        const unsigned shelf = i / 128u;
        if (col % 2u != 0 || shelf % 2u != 0)
            continue;
        write_pattern(*a, ids[i], i);
        ++written;
        if (written == lx::gfx::k_atlas_dirty_rects)
            LUMEN_CHECK(a->dirty_rects(0).size() == lx::gfx::k_atlas_dirty_rects &&
                        !a->dirty_full(0));
    }
    LUMEN_CHECK(written > lx::gfx::k_atlas_dirty_rects);
    LUMEN_CHECK(a->dirty_full(0) && a->dirty_rects(0).empty());
}

LUMEN_TEST(caps_are_fixed) {
    auto a = std::make_unique<atlas>();
    LUMEN_CHECK(!a->allocate(atlas_format::r8, 0, 4));
    LUMEN_CHECK(!a->allocate(atlas_format::r8, lx::gfx::k_atlas_page_max + 1, 4));
    // Two RGBA8 pages of 2048², then refusal (every tile is live).
    LUMEN_CHECK(static_cast<bool>(a->allocate(atlas_format::rgba8, 2048, 2048)));
    LUMEN_CHECK(static_cast<bool>(a->allocate(atlas_format::rgba8, 2048, 2048)));
    LUMEN_CHECK(!a->allocate(atlas_format::rgba8, 1, 1));
    // Four R8 pages.
    for (unsigned i = 0; i < lx::gfx::k_atlas_r8_pages; ++i)
        LUMEN_CHECK(static_cast<bool>(a->allocate(atlas_format::r8, 2048, 2048)));
    LUMEN_CHECK(!a->allocate(atlas_format::r8, 1, 1));
    LUMEN_CHECK(a->stats().pages == lx::gfx::k_atlas_pages);

    // The tile table.
    auto b = std::make_unique<atlas>();
    std::vector<atlas_tile_id> ids;
    for (unsigned i = 0; i < lx::gfx::k_atlas_max_tiles; ++i) {
        auto id = b->allocate(atlas_format::r8, 1, 1);
        LUMEN_CHECK(static_cast<bool>(id));
        ids.push_back(*id);
    }
    LUMEN_CHECK(!b->allocate(atlas_format::r8, 1, 1));
    b->release(ids[5]);
    auto again = b->allocate(atlas_format::r8, 1, 1);
    LUMEN_CHECK(static_cast<bool>(again));
    // The slot was reused under a new generation: the old id stays dead.
    atlas_tile t{};
    LUMEN_CHECK(!b->lookup(ids[5], t) && b->lookup(*again, t));
}

namespace {

/// Pages of [p, p + bytes) resident in this process, by mincore.
[[nodiscard]] std::size_t resident_pages(const void* p, std::size_t bytes) {
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    std::vector<unsigned char> v((bytes + page - 1) / page);
    if (::mincore(const_cast<void*>(p), bytes, v.data()) != 0)
        return ~std::size_t{0};
    std::size_t n = 0;
    for (unsigned char c : v)
        n += c & 1u;
    return n;
}

} // namespace

// A page is a 4 MiB mapping (R8 at its 2048² maximum) but costs memory only where tiles
// are written: the compositor's title glyphs touch a few hundred KB of it. A rebuild hands
// back the rows it empties, and its scratch is gone when it returns.
LUMEN_TEST(page_memory_is_resident_only_where_written) {
    const std::size_t page_bytes = std::size_t{lx::gfx::k_atlas_page_max} * lx::gfx::k_atlas_page_max;
    auto a = std::make_unique<atlas>();
    a->begin_frame(1);
    auto first = a->allocate(atlas_format::r8, 16, 16);
    LUMEN_CHECK(static_cast<bool>(first));
    write_pattern(*a, *first, 7);
    const auto view = a->page(0);
    LUMEN_CHECK(view.valid());
    const std::size_t one_tile = resident_pages(view.pixels, page_bytes);
    std::printf("    one 16x16 tile: %zu resident pages of the 4 MiB page\n", one_tile);
    LUMEN_CHECK(one_tile >= 1 && one_tile <= 16);

    // Fill most of the page with glyphs, then let all but a few go stale and rebuild.
    test_rng rng{};
    std::vector<atlas_tile_id> ids;
    for (unsigned i = 0; i < 6000; ++i) {
        unsigned w = 0;
        unsigned h = 0;
        glyph_box(rng, w, h);
        auto id = a->allocate(atlas_format::r8, w, h);
        if (!id || a->page(0).pixels != view.pixels)
            break;
        atlas_tile t{};
        if (!a->lookup(*id, t) || t.page != 0)
            break;
        write_pattern(*a, *id, i);
        ids.push_back(*id);
    }
    const std::size_t filled = resident_pages(view.pixels, page_bytes);
    for (std::uint64_t f = 2; f <= 200; ++f) {
        a->begin_frame(f);
        for (unsigned i = 0; i < ids.size(); i += 50)
            a->touch(ids[i]);
    }
    auto r = a->rebuild(0);
    LUMEN_CHECK(static_cast<bool>(r));
    const std::size_t after = resident_pages(view.pixels, page_bytes);
    std::printf("    %zu tiles: %zu resident pages; after a rebuild keeping %u: %zu\n",
                ids.size(), filled, r ? r->moved : 0u, after);
    LUMEN_CHECK(after * 4 < filled);
    for (unsigned i = 0; i < ids.size(); i += 50)
        LUMEN_CHECK(pattern_intact(*a, ids[i], i));
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
