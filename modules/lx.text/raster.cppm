module;

#if defined(LUMEN_HAS_TEXT)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#endif

export module lx.text:raster;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.trace;

import :faces;

export namespace lx::text {

// ── Glyph raster into the atlas (ADR-010 §4.6 `:raster`) ─────────────────────────
//
// `glyph_cache` maps (face id, size × scale, subpixel x variant, glyph id, hinting) to a tile
// of an `lx.gfx:atlas` plus the bitmap's bearing, rasterising a glyph with FreeType the first
// time it is asked for. The rules (each a golden-image gate, ADR §8.4):
//
// - 4 horizontal subpixel variants (GPUI's SUBPIXEL_VARIANTS): a pen x is rounded to a quarter
//   pixel; its whole pixels place the sprite and the quarter picks the variant, rasterised with
//   the outline moved right by variant / 4 px. The vertical position snaps to whole pixels.
//   `glyph_pen` is the rounding every emitter uses.
// - Unhinted outlines, grayscale antialiasing (FT_RENDER_MODE_NORMAL) by default;
//   `glyph_hinting::light` is FreeType's light autohinter (vertical only), which a theme may
//   select for scales below 1.5 (`effective_hinting`).
// - A face with colour glyphs (FT_HAS_COLOR: COLRv0 layers, CBDT / sbix bitmaps) is loaded
//   with FT_LOAD_COLOR; a glyph that comes back as BGRA goes to an RGBA8 page (premultiplied,
//   R, G, B, A bytes) to be drawn as a `poly_sprite`. Colour glyphs have no subpixel
//   variants: they are rasterised once, at variant 0, and every variant's lookup answers with
//   that tile (the emitter places them on whole pixels). A colour face's plain glyphs take the
//   outline path. Bitmap-only colour faces need a strike FreeType can size to; COLRv0 scales
//   to any size.
//
// Storage: a set-associative table of `k_glyph_cache_ways` ways per set, fixed at
// construction, each way a key, the tile id and the bitmap's placement. A full set replaces
// its least recently used way (`overflow_action::drop_oldest`, the default) and releases that
// way's tile when it was not used in the atlas's current frame (a tile drawn this frame stays
// until the atlas ages it out); `reject` / `warn_and_drop` keep the set and fail the new glyph
// (`warn_and_drop` warns once). Rasterising writes into a scratch bitmap of
// `max_glyph_px`² RGBA texels allocated at construction, so a miss allocates nothing beyond
// FreeType's own glyph-slot work; a glyph larger than that fails (counted). Nothing is
// allocated on a hit.
//
// Atlas tiles are looked up every frame: `place` returns where the tile is now (a rebuild at
// `atlas::begin_frame` moves tiles; the id stays) and touches it for the 120-frame eviction
// rule. A tile the atlas evicted makes `find` miss, and the next `get_locked` rasterises the
// glyph again into the same way (`counters().rerasterized`).
//
// Threading contract: UI affinity, like the atlas it writes (`set_owner_affinity` asserts
// another owner; `any`, the default, checks nothing). A miss uses a face, so `get_locked`
// expects `faces.lock()` held; `find` and `place` take no lock.

inline constexpr unsigned k_subpixel_variants = 4;
inline constexpr unsigned k_glyph_cache_ways = 8;
inline constexpr unsigned k_glyph_cache_entries = 16384;
/// The largest glyph bitmap edge rasterised (the scratch is this squared × 4 bytes).
inline constexpr unsigned k_glyph_max_px = 256;

enum class glyph_hinting : std::uint8_t {
    none,  ///< unhinted outlines (the default)
    light, ///< FreeType's light autohinter: vertical snapping only
};

/// The hinting a glyph is rasterised with: a theme's choice applies below scale 1.5 only.
[[nodiscard]] constexpr glyph_hinting effective_hinting(glyph_hinting theme, float scale) {
    return theme == glyph_hinting::light && scale < 1.5f ? glyph_hinting::light
                                                         : glyph_hinting::none;
}

/// A pen position split for placement: whole device pixels and the subpixel variant.
struct glyph_pen_pos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t variant = 0;
};

/// Rounds a pen (26.6 device px) the way glyphs are rasterised: x to the nearest quarter
/// pixel (floor of the whole pixels, the quarter as the variant), y to the nearest pixel.
[[nodiscard]] constexpr glyph_pen_pos glyph_pen(std::int32_t x_26_6, std::int32_t y_26_6) {
    const std::int32_t quarters = (x_26_6 + 8) >> 4; // arithmetic shift: floor
    return {quarters >> 2, (y_26_6 + 32) >> 6, static_cast<std::uint8_t>(quarters & 3)};
}

struct glyph_key {
    face_id face{};
    /// The em size the glyph is rasterised at (font size × scale), 26.6 device px.
    std::int32_t size_26_6 = 0;
    std::uint32_t glyph = 0;
    std::uint8_t variant = 0; ///< 0 – k_subpixel_variants − 1
    glyph_hinting hinting = glyph_hinting::none;
    friend constexpr bool operator==(const glyph_key&, const glyph_key&) = default;
};

/// A rasterised glyph: its tile and where the bitmap sits relative to the pen.
struct glyph_entry {
    lx::gfx::atlas_tile_id tile{};
    /// Pen (whole pixels) → the bitmap's first column.
    std::int16_t left = 0;
    /// Baseline → the bitmap's first row, up positive (FreeType's bitmap_top).
    std::int16_t top = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    /// An RGBA8 tile: draw it as a `poly_sprite`.
    bool color = false;

    /// Nothing to draw (a space): no tile.
    [[nodiscard]] constexpr bool empty() const { return width == 0 || height == 0; }
};

struct glyph_cache_config {
    /// Ways in all; rounded down to a power-of-two number of sets of `k_glyph_cache_ways`.
    unsigned entries = k_glyph_cache_entries;
    unsigned max_glyph_px = k_glyph_max_px;
    lx::runtime::overflow_action overflow = lx::runtime::overflow_action::drop_oldest;
};

struct glyph_cache_counters {
    /// `find` / `get` answered from the table with a live tile (or an empty glyph).
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    /// Glyphs rasterised and written to the atlas (each is FreeType work on that frame).
    std::uint64_t rasterized = 0;
    /// Of those, colour (RGBA8) glyphs.
    std::uint64_t color = 0;
    /// Glyphs with no ink (cached without a tile).
    std::uint64_t empty = 0;
    /// Misses on an entry whose tile the atlas had evicted (rasterised again).
    std::uint64_t rerasterized = 0;
    /// Ways replaced in a full set, and the tiles released with them.
    std::uint64_t evictions = 0;
    std::uint64_t tiles_released = 0;
    /// Glyphs that could not be cached: a full set without `drop_oldest`.
    std::uint64_t overflows = 0;
    /// FreeType could not load or render, the atlas had no room, or the bitmap was too large.
    std::uint64_t failures = 0;
    std::uint64_t oversize = 0;
    unsigned live = 0;
    unsigned capacity = 0;
};

class glyph_cache {
public:
    explicit glyph_cache(lx::gfx::atlas& atlas, glyph_cache_config config = {});
    ~glyph_cache();
    glyph_cache(const glyph_cache&) = delete;
    glyph_cache& operator=(const glyph_cache&) = delete;

    [[nodiscard]] lx::gfx::atlas& atlas() const { return *atlas_; }

    /// The cached glyph when its tile is still in the atlas (or it is empty); null otherwise.
    /// No lock, no FreeType. A colour glyph answers every variant.
    [[nodiscard]] const glyph_entry* find(const glyph_key& key);
    /// `find`, else rasterises the glyph into the atlas. The caller holds `faces.lock()`.
    /// Errors: io (the face is not loaded), invalid_argument (FreeType refused the glyph, it
    /// is larger than `max_glyph_px`, the atlas is full, or its set is full without
    /// `drop_oldest`), not_implemented without FreeType. The entry stays valid until the
    /// next `get*` or `clear`.
    [[nodiscard]] lx::result<const glyph_entry*> get_locked(face_cache& faces,
                                                            const glyph_key& key);
    /// `get_locked`, taking `faces.lock()` only on a miss.
    [[nodiscard]] lx::result<const glyph_entry*> get(face_cache& faces, const glyph_key& key);
    /// Where the entry's tile is now, stamped used this atlas frame. False for an empty glyph
    /// or a tile the atlas evicted (call `get*` again).
    [[nodiscard]] bool place(const glyph_entry& entry, lx::gfx::atlas_tile& out) const;

    /// Releases every tile and empties the table.
    void clear();
    [[nodiscard]] glyph_cache_counters counters() const;
    void set_owner_affinity(lx::runtime::affinity owner) { owner_ = owner; }

private:
    struct way {
        glyph_key key{};
        glyph_entry entry{};
        std::uint64_t used = 0;  ///< LRU clock
        std::uint64_t frame = 0; ///< atlas frame of the last use
        bool live = false;
    };

    [[nodiscard]] static std::uint64_t hash(const glyph_key& k);
    [[nodiscard]] way* set_of(const glyph_key& k);
    /// The way holding `k`, or null.
    [[nodiscard]] way* probe(const glyph_key& k);
    [[nodiscard]] bool alive(const way& w) const;
    void release_tile(way& w);
    /// Rasterises `key` into `out` (tile allocated and written). Caller holds the lock.
    [[nodiscard]] lx::result<void> rasterize_locked(face_cache& faces, const glyph_key& key,
                                                    glyph_entry& out, bool& color_out);

    lx::gfx::atlas* atlas_ = nullptr;
    glyph_cache_config config_{};
    std::unique_ptr<way[]> ways_{};
    std::unique_ptr<std::uint8_t[]> scratch_{};
    unsigned sets_ = 0;
    std::uint64_t clock_ = 0;
    glyph_cache_counters counters_{};
    lx::runtime::affinity owner_ = lx::runtime::affinity::any;
    bool warned_ = false;
};

} // namespace lx::text

// ── Implementation ────────────────────────────────────────────────────────────

lx::text::glyph_cache::glyph_cache(lx::gfx::atlas& atlas, glyph_cache_config config)
    : atlas_{&atlas}, config_{config} {
    unsigned sets = config_.entries / k_glyph_cache_ways;
    if (sets == 0)
        sets = 1;
    sets_ = std::bit_floor(sets);
    config_.entries = sets_ * k_glyph_cache_ways;
    if (config_.max_glyph_px == 0)
        config_.max_glyph_px = 1;
    ways_ = std::make_unique<way[]>(config_.entries);
    scratch_ = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(config_.max_glyph_px) *
                                                config_.max_glyph_px * 4u);
    counters_.capacity = config_.entries;
}

lx::text::glyph_cache::~glyph_cache() { clear(); }

std::uint64_t lx::text::glyph_cache::hash(const glyph_key& k) {
    // FNV-1a over the key's fields, then a final mix so the low bits (the set) spread.
    std::uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](std::uint64_t v) {
        for (unsigned i = 0; i < 8; ++i) {
            h = (h ^ (v & 0xFFu)) * 1099511628211ull;
            v >>= 8u;
        }
    };
    mix(k.face.value);
    mix(static_cast<std::uint32_t>(k.size_26_6));
    mix(k.glyph);
    mix((static_cast<std::uint64_t>(k.variant) << 8u) | static_cast<std::uint64_t>(k.hinting));
    h ^= h >> 29u;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32u;
    return h;
}

lx::text::glyph_cache::way* lx::text::glyph_cache::set_of(const glyph_key& k) {
    return ways_.get() + (hash(k) & (sets_ - 1u)) * k_glyph_cache_ways;
}

lx::text::glyph_cache::way* lx::text::glyph_cache::probe(const glyph_key& k) {
    way* set = set_of(k);
    for (unsigned i = 0; i < k_glyph_cache_ways; ++i)
        if (set[i].live && set[i].key == k)
            return &set[i];
    return nullptr;
}

bool lx::text::glyph_cache::alive(const way& w) const {
    if (w.entry.empty())
        return true;
    lx::gfx::atlas_tile t{};
    return atlas_->lookup(w.entry.tile, t);
}

void lx::text::glyph_cache::release_tile(way& w) {
    if (!w.entry.tile.valid())
        return;
    lx::gfx::atlas_tile t{};
    // Released only when nothing of the atlas's current frame can be drawing it: a sprite
    // emitted this frame names its texels until the frame is built.
    if (atlas_->lookup(w.entry.tile, t) && w.frame < atlas_->frame()) {
        atlas_->release(w.entry.tile);
        ++counters_.tiles_released;
    }
    w.entry.tile = {};
}

const lx::text::glyph_entry* lx::text::glyph_cache::find(const glyph_key& key) {
    lx::runtime::assert_affinity(owner_);
    way* w = probe(key);
    if (!w && key.variant != 0) {
        // A colour glyph is cached once, at variant 0, for every variant.
        glyph_key base = key;
        base.variant = 0;
        way* b = probe(base);
        if (b && b->entry.color)
            w = b;
    }
    if (!w || !alive(*w)) {
        ++counters_.misses;
        return nullptr;
    }
    w->used = ++clock_;
    w->frame = atlas_->frame();
    ++counters_.hits;
    return &w->entry;
}

lx::result<const lx::text::glyph_entry*> lx::text::glyph_cache::get(face_cache& faces,
                                                                     const glyph_key& key) {
    if (const glyph_entry* e = find(key))
        return e;
    auto lock = faces.lock();
    return get_locked(faces, key);
}

lx::result<const lx::text::glyph_entry*>
lx::text::glyph_cache::get_locked(face_cache& faces, const glyph_key& key) {
    lx::runtime::assert_affinity(owner_);
    if (const glyph_entry* e = find(key))
        return e;
    if (key.variant >= k_subpixel_variants)
        return lx::invalid_argument("glyph_cache: subpixel variant out of range");

    glyph_entry e{};
    bool color = false;
    if (const lx::result<void> r = rasterize_locked(faces, key, e, color); !r) {
        ++counters_.failures;
        return r.get_error();
    }
    // A colour glyph is stored under variant 0, which answers every variant.
    glyph_key stored = key;
    if (color)
        stored.variant = 0;

    // The way to fill: the key's own (its tile was evicted), else a free one, else the set's
    // least recently used.
    way* target = probe(stored);
    if (target) {
        ++counters_.rerasterized;
        release_tile(*target);
    } else {
        way* set = set_of(stored);
        for (unsigned i = 0; i < k_glyph_cache_ways && !target; ++i)
            if (!set[i].live)
                target = &set[i];
        if (!target) {
            if (config_.overflow != lx::runtime::overflow_action::drop_oldest) {
                ++counters_.overflows;
                if (config_.overflow == lx::runtime::overflow_action::warn_and_drop && !warned_) {
                    warned_ = true;
                    lx::trace::logger::global().log(lx::trace::level::warn, "text",
                                                    "glyph_cache: set full, glyph not cached");
                }
                // The tile just written is not referenced by anything: give it back.
                if (e.tile.valid())
                    atlas_->release(e.tile);
                return lx::invalid_argument("glyph_cache: set full");
            }
            target = &set[0];
            for (unsigned i = 1; i < k_glyph_cache_ways; ++i)
                if (set[i].used < target->used)
                    target = &set[i];
            release_tile(*target);
            ++counters_.evictions;
        } else {
            ++counters_.live;
        }
    }
    target->live = true;
    target->key = stored;
    target->entry = e;
    target->used = ++clock_;
    target->frame = atlas_->frame();
    return &target->entry;
}

bool lx::text::glyph_cache::place(const glyph_entry& entry, lx::gfx::atlas_tile& out) const {
    if (entry.empty() || !atlas_->lookup(entry.tile, out))
        return false;
    atlas_->touch(entry.tile);
    return true;
}

void lx::text::glyph_cache::clear() {
    if (!ways_)
        return;
    for (unsigned i = 0; i < config_.entries; ++i) {
        way& w = ways_[i];
        if (w.live && w.entry.tile.valid()) {
            lx::gfx::atlas_tile t{};
            if (atlas_->lookup(w.entry.tile, t))
                atlas_->release(w.entry.tile);
        }
        w = way{};
    }
    counters_.live = 0;
}

lx::text::glyph_cache_counters lx::text::glyph_cache::counters() const { return counters_; }

lx::result<void> lx::text::glyph_cache::rasterize_locked(face_cache& faces, const glyph_key& key,
                                                         glyph_entry& out, bool& color_out) {
#if defined(LUMEN_HAS_TEXT)
    const face_cache::handles h = faces.use_locked(key.face, key.size_26_6);
    auto* face = static_cast<FT_Face>(h.ft_face);
    if (!face)
        return lx::make_error(lx::io_err::not_found, "glyph_cache: face not loaded");
    const FT_Int32 hint_flags = key.hinting == glyph_hinting::light
                                    ? (FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT)
                                    : FT_LOAD_NO_HINTING;
    FT_GlyphSlot slot = nullptr;
    bool color = false;
    if (FT_HAS_COLOR(face) &&
        FT_Load_Glyph(face, key.glyph, hint_flags | FT_LOAD_COLOR | FT_LOAD_RENDER) == 0 &&
        face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        slot = face->glyph;
        color = true;
    }
    if (!slot) {
        if (FT_Load_Glyph(face, key.glyph, hint_flags | FT_LOAD_NO_BITMAP) != 0)
            return lx::invalid_argument("glyph_cache: FreeType could not load the glyph");
        slot = face->glyph;
        if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
            // The subpixel variant: the outline moved right by variant / 4 px (26.6).
            if (key.variant != 0)
                FT_Outline_Translate(&slot->outline, static_cast<FT_Pos>(key.variant) * 16, 0);
            if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0)
                return lx::invalid_argument("glyph_cache: FreeType could not render the glyph");
        } else if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
            return lx::invalid_argument("glyph_cache: glyph format not supported");
        }
    }
    const FT_Bitmap& bm = slot->bitmap;
    out = {};
    out.left = static_cast<std::int16_t>(slot->bitmap_left);
    out.top = static_cast<std::int16_t>(slot->bitmap_top);
    color_out = color;
    if (bm.width == 0 || bm.rows == 0) {
        ++counters_.empty;
        return {};
    }
    if (bm.width > config_.max_glyph_px || bm.rows > config_.max_glyph_px) {
        ++counters_.oversize;
        return lx::invalid_argument("glyph_cache: glyph bitmap larger than max_glyph_px");
    }
    const unsigned w = bm.width;
    const unsigned rows = bm.rows;
    const int pitch = bm.pitch;
    const auto row_of = [&](unsigned y) -> const std::uint8_t* {
        // A negative pitch stores the rows bottom-up.
        return pitch >= 0 ? bm.buffer + static_cast<std::size_t>(y) * pitch
                          : bm.buffer + static_cast<std::size_t>(rows - 1u - y) * -pitch;
    };
    std::uint8_t* px = scratch_.get();
    lx::gfx::atlas_format format = lx::gfx::atlas_format::r8;
    unsigned stride = w;
    if (color) {
        // FreeType's BGRA is premultiplied, B, G, R, A in memory: the atlas wants R, G, B, A.
        format = lx::gfx::atlas_format::rgba8;
        stride = w * 4u;
        for (unsigned y = 0; y < rows; ++y) {
            const std::uint8_t* src = row_of(y);
            std::uint8_t* dst = px + static_cast<std::size_t>(y) * stride;
            for (unsigned x = 0; x < w; ++x) {
                dst[x * 4u + 0] = src[x * 4u + 2];
                dst[x * 4u + 1] = src[x * 4u + 1];
                dst[x * 4u + 2] = src[x * 4u + 0];
                dst[x * 4u + 3] = src[x * 4u + 3];
            }
        }
    } else if (bm.pixel_mode == FT_PIXEL_MODE_GRAY && bm.num_grays == 256) {
        for (unsigned y = 0; y < rows; ++y)
            std::memcpy(px + static_cast<std::size_t>(y) * stride, row_of(y), w);
    } else if (bm.pixel_mode == FT_PIXEL_MODE_MONO) {
        for (unsigned y = 0; y < rows; ++y) {
            const std::uint8_t* src = row_of(y);
            for (unsigned x = 0; x < w; ++x)
                px[static_cast<std::size_t>(y) * stride + x] =
                    (src[x >> 3u] & (0x80u >> (x & 7u))) ? 255u : 0u;
        }
    } else {
        return lx::invalid_argument("glyph_cache: bitmap pixel mode not supported");
    }
    auto tile = atlas_->insert(format, w, rows,
                               {px, static_cast<std::size_t>(stride) * rows}, stride);
    if (!tile)
        return tile.get_error();
    out.tile = *tile;
    out.width = static_cast<std::uint16_t>(w);
    out.height = static_cast<std::uint16_t>(rows);
    out.color = color;
    ++counters_.rasterized;
    if (color)
        ++counters_.color;
    return {};
#else
    (void)faces;
    (void)key;
    (void)out;
    (void)color_out;
    return lx::not_implemented("lx::text::glyph_cache::get_locked");
#endif
}
