module;

import lx.foundation;
import lx.runtime;

export module lx.gfx:atlas;

import std;

import :primitives;

export namespace lx::gfx {

// ── Glyph and icon atlas (ADR-010 §4.6) ─────────────────────────────────────────
//
// Tiles of R8 coverage (glyphs, icons, path coverage) and RGBA8 premultiplied colour
// (images, colour emoji) packed into a few large pages. Pixels live in CPU memory owned by
// the atlas: the CPU renderer samples them in place, and a GPU backend uploads what each
// page's dirty-rect list names. UI affinity: tiles are allocated, written and touched by
// the frame build.
//
// Packing is shelf packing with bucketed shelf heights (étagère's scheme): a tile's height
// rounds up to its bucket, a shelf holds tiles of one bucket left to right across the page,
// shelves stack top to bottom. A page starts at 1024² and grows once to 2048² (every shelf
// gains the new width, new shelves go below); pages are created on demand up to 4 R8 and 2
// RGBA8. When nothing fits, tiles unused for `k_atlas_stale_frames` frames are freed in
// place, and a page that was above `k_atlas_rebuild_occupancy` with stale tiles is marked
// for a rebuild (its live tiles repacked, the page then whole-page dirty), which runs at the
// next `begin_frame` — never inside `allocate`: a rebuild moves live tiles, and a uv already
// looked up for the current frame (a sprite emitted before the allocation) must stay where
// its pixels are until the frame is built. A tile used in the last 120 frames is never
// evicted.
//
// Fixed caps everywhere: the tile table, the shelves of a page and the dirty list are
// fixed arrays sized at construction; after a page exists nothing allocates except the
// creation of another page (and, with the first page of a format, the rebuild scratch).

enum class atlas_format : std::uint8_t {
    r8,    ///< one byte of coverage per texel
    rgba8, ///< R, G, B, A bytes per texel (in that memory order), premultiplied, sRGB-encoded
};

inline constexpr unsigned k_atlas_page_initial = 1024;
inline constexpr unsigned k_atlas_page_max = 2048;
inline constexpr unsigned k_atlas_r8_pages = 4;
inline constexpr unsigned k_atlas_rgba8_pages = 2;
/// Page ids are global: 0–3 are R8, 4–5 RGBA8. A sprite instance's `page` is this id.
inline constexpr unsigned k_atlas_pages = k_atlas_r8_pages + k_atlas_rgba8_pages;
inline constexpr unsigned k_atlas_first_rgba8_page = k_atlas_r8_pages;
/// Live tiles over every page.
inline constexpr unsigned k_atlas_max_tiles = 16384;
/// Shelves per page: a 2048-high page of the smallest (2 px) bucket.
inline constexpr unsigned k_atlas_max_shelves = 1024;
/// Dirty regions per page; one more makes the whole page dirty.
inline constexpr unsigned k_atlas_dirty_rects = 256;
/// A tile last used this many frames ago (or longer) may be evicted.
inline constexpr unsigned k_atlas_stale_frames = 120;
/// Above this share of a page claimed by shelves, reclaiming stale tiles rebuilds it.
inline constexpr float k_atlas_rebuild_occupancy = 0.85f;

/// Where a tile is: a page and a texel box. `uv()` is what a sprite instance carries.
struct atlas_tile {
    std::uint8_t page = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;

    [[nodiscard]] constexpr sprite_uv uv() const {
        return {x, y, static_cast<std::uint16_t>(x + w), static_cast<std::uint16_t>(y + h)};
    }
};

/// A tile handle: stays valid across rebuilds (the tile moves, the id does not) until the
/// tile is released or evicted; a stale id then fails `lookup`. 0 is no tile.
struct atlas_tile_id {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(atlas_tile_id, atlas_tile_id) = default;
};

/// A page's pixels for reading (the CPU renderer) or upload (GPU backends). `size` is the
/// page's current edge in texels; rows are `stride` bytes apart.
struct atlas_page_view {
    const std::uint8_t* pixels = nullptr;
    unsigned stride = 0;
    unsigned size = 0;
    atlas_format format = atlas_format::r8;
    [[nodiscard]] bool valid() const { return pixels != nullptr && size > 0; }
};

struct atlas_rebuild_stats {
    unsigned page = 0;
    unsigned evicted = 0;  ///< stale tiles dropped before the repack
    unsigned moved = 0;    ///< live tiles repacked
    unsigned lost = 0;     ///< live tiles that no longer fit (released; never seen in practice)
    float occupancy_before = 0.f;
    float occupancy_after = 0.f;
    std::uint64_t ns = 0;  ///< wall time of the synchronous rebuild (CLOCK_MONOTONIC)
};

struct atlas_stats {
    unsigned pages = 0;
    unsigned live_tiles = 0;
    unsigned long long evicted = 0;  ///< stale tiles reclaimed, in place or by a rebuild
    unsigned long long rebuilds = 0;
    unsigned long long rebuild_lost = 0;
    std::uint64_t last_rebuild_ns = 0;
    std::uint64_t max_rebuild_ns = 0;
    unsigned long long grows = 0;
};

class atlas {
public:
    atlas();
    ~atlas();
    atlas(const atlas&) = delete;
    atlas& operator=(const atlas&) = delete;

    /// The frame number tiles are stamped with (`touch`, `allocate`); monotonic. Runs the
    /// rebuilds `allocate` marked since the last call, before any emitter of the new frame
    /// looks up a uv (call it before building the frame).
    void begin_frame(std::uint64_t frame);
    [[nodiscard]] std::uint64_t frame() const { return frame_; }

    /// A `w` × `h` tile of `format`, stamped used this frame. Fails (invalid_argument) for
    /// an empty tile, one larger than a page, or when every page of the format is full of
    /// tiles used within the last 120 frames, or the tile table is full.
    [[nodiscard]] lx::result<atlas_tile_id> allocate(atlas_format format, unsigned w, unsigned h);
    /// Copies the tile's pixels from `pixels` (`stride` bytes per row, `w` × bytes-per-texel
    /// used of each) into its page and adds the tile's rect to the page's dirty list.
    [[nodiscard]] lx::result<void> write(atlas_tile_id id, std::span<const std::uint8_t> pixels,
                                         unsigned stride);
    /// `allocate` + `write`.
    [[nodiscard]] lx::result<atlas_tile_id> insert(atlas_format format, unsigned w, unsigned h,
                                                   std::span<const std::uint8_t> pixels,
                                                   unsigned stride);
    /// The tile's current place; false for an id released, evicted or never issued.
    [[nodiscard]] bool lookup(atlas_tile_id id, atlas_tile& out) const;
    /// Stamps the tile used this frame (what the emitter does for every sprite it draws).
    void touch(atlas_tile_id id);
    /// Frees the tile now; its id goes stale.
    void release(atlas_tile_id id);

    /// Frees, in place, every tile last used `k_atlas_stale_frames` or more frames ago.
    /// Returns how many.
    unsigned evict_stale();
    /// Drops the page's stale tiles and repacks the live ones from the top (tallest bucket
    /// first), synchronously; the whole page becomes dirty. Moves live tiles: call it between
    /// frames (as `begin_frame` does), never after a uv of the frame being built was looked
    /// up. Fails for a page not created.
    [[nodiscard]] lx::result<atlas_rebuild_stats> rebuild(unsigned page);

    [[nodiscard]] static constexpr atlas_format page_format(unsigned page) {
        return page < k_atlas_first_rgba8_page ? atlas_format::r8 : atlas_format::rgba8;
    }
    [[nodiscard]] atlas_page_view page(unsigned page) const;
    /// Share of the page's area claimed by shelves (tiles and the holes evicted tiles left
    /// before their shelf's end), 0 for a page not created.
    [[nodiscard]] float occupancy(unsigned page) const;
    /// Sum of live tile areas over the page's area.
    [[nodiscard]] float density(unsigned page) const;
    [[nodiscard]] unsigned live_tiles(unsigned page) const;
    [[nodiscard]] unsigned stale_tiles(unsigned page) const;
    /// `allocate` reclaimed stale tiles of this page in place while it was above
    /// `k_atlas_rebuild_occupancy`: the next `begin_frame` rebuilds it.
    [[nodiscard]] bool rebuild_pending(unsigned page) const {
        return page < k_atlas_pages && pages_[page].rebuild_pending;
    }

    /// What changed since `clear_dirty`: coalesced texel rects (empty when `dirty_full`).
    [[nodiscard]] std::span<const lx::rect2i> dirty_rects(unsigned page) const;
    /// The whole page must be uploaded: the dirty list overflowed, the page grew or was
    /// rebuilt.
    [[nodiscard]] bool dirty_full(unsigned page) const;
    void clear_dirty(unsigned page);

    [[nodiscard]] atlas_stats stats() const;
    [[nodiscard]] static constexpr unsigned bucket_height(unsigned h) {
        if (h <= 32)
            return (h + 1u) & ~1u;
        if (h <= 64)
            return (h + 3u) & ~3u;
        if (h <= 128)
            return (h + 7u) & ~7u;
        return (h + 15u) & ~15u;
    }

private:
    struct tile_entry {
        std::uint64_t last_used = 0;
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint16_t w = 0;
        std::uint16_t h = 0;
        std::uint16_t shelf = 0;
        std::uint16_t generation = 1;
        std::uint8_t page = 0;
        bool live = false;
    };
    struct shelf {
        std::uint16_t y = 0;
        std::uint16_t height = 0;
        std::uint16_t tail = 0; ///< first free x
        std::uint16_t live = 0; ///< live tiles on it
    };
    struct page_state {
        std::uint8_t* pixels = nullptr; ///< `k_atlas_page_max`² texels, calloc'd
        unsigned stride = 0;
        unsigned size = 0; ///< 0: not created
        unsigned next_y = 0;
        unsigned shelf_count = 0;
        shelf* shelves = nullptr; ///< `k_atlas_max_shelves`, inside `storage`
        lx::rect2i dirty[k_atlas_dirty_rects]{};
        unsigned dirty_count = 0;
        bool dirty_full = false;
        bool rebuild_pending = false; ///< rebuilt at the next `begin_frame`
        unsigned live = 0;
        unsigned long long live_area = 0;
    };
    struct storage;

    [[nodiscard]] static unsigned bytes_per_texel(atlas_format f) {
        return f == atlas_format::r8 ? 1u : 4u;
    }
    [[nodiscard]] tile_entry* entry(atlas_tile_id id);
    [[nodiscard]] const tile_entry* entry(atlas_tile_id id) const;
    [[nodiscard]] bool create_page(unsigned page);
    /// Places a `w` × `h` tile on `page` without growing it; false when it does not fit.
    [[nodiscard]] bool place(unsigned page, unsigned w, unsigned h, unsigned& shelf_out,
                             unsigned& x_out, unsigned& y_out);
    [[nodiscard]] bool grow(unsigned page);
    void free_tile(unsigned index);
    [[nodiscard]] bool reclaim(atlas_format format);
    void add_dirty(unsigned page, lx::rect2i r);
    [[nodiscard]] bool stale(const tile_entry& t) const {
        return frame_ >= t.last_used && frame_ - t.last_used >= k_atlas_stale_frames;
    }

    storage* store_ = nullptr;
    page_state pages_[k_atlas_pages]{};
    std::uint8_t* scratch_ = nullptr; ///< rebuild scratch, one page of the largest format
    std::size_t scratch_bytes_ = 0;
    std::uint64_t frame_ = 0;
    unsigned free_top_ = 0;
    atlas_stats stats_{};
};

} // namespace lx::gfx


struct lx::gfx::atlas::storage {
    tile_entry tiles[k_atlas_max_tiles]{};
    /// Free tile indices, a stack.
    std::uint16_t free_list[k_atlas_max_tiles]{};
    shelf shelves[k_atlas_pages][k_atlas_max_shelves]{};
    /// Rebuild scratch: the page's live tiles in repack order.
    std::uint16_t order[k_atlas_max_tiles]{};
    std::uint32_t scratch_offset[k_atlas_max_tiles]{};
};

namespace {

[[nodiscard]] std::uint64_t lx_gfx_atlas_now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] long long lx_gfx_atlas_area(lx::rect2i r) {
    return static_cast<long long>(r.width) * r.height;
}

[[nodiscard]] lx::rect2i lx_gfx_atlas_bbox(lx::rect2i a, lx::rect2i b) {
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    const int y1 = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
    return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace

lx::gfx::atlas::atlas() {
    store_ = new storage{};
    // Indices handed out low first: the stack holds them high to low.
    for (unsigned i = 0; i < k_atlas_max_tiles; ++i)
        store_->free_list[i] = static_cast<std::uint16_t>(k_atlas_max_tiles - 1u - i);
    free_top_ = k_atlas_max_tiles;
    for (unsigned p = 0; p < k_atlas_pages; ++p)
        pages_[p].shelves = store_->shelves[p];
}

lx::gfx::atlas::~atlas() {
    for (auto& p : pages_)
        std::free(p.pixels);
    std::free(scratch_);
    delete store_;
}

void lx::gfx::atlas::begin_frame(std::uint64_t frame) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (frame > frame_)
        frame_ = frame;
    // Rebuilds `allocate` asked for: between frames, before any uv of this one is looked up.
    for (unsigned p = 0; p < k_atlas_pages; ++p)
        if (pages_[p].rebuild_pending) {
            pages_[p].rebuild_pending = false;
            (void)rebuild(p);
        }
}

lx::gfx::atlas::tile_entry* lx::gfx::atlas::entry(atlas_tile_id id) {
    const unsigned index = (id.value & 0xFFFFu);
    if (index == 0 || index > k_atlas_max_tiles)
        return nullptr;
    tile_entry& t = store_->tiles[index - 1u];
    if (!t.live || t.generation != static_cast<std::uint16_t>(id.value >> 16u))
        return nullptr;
    return &t;
}

const lx::gfx::atlas::tile_entry* lx::gfx::atlas::entry(atlas_tile_id id) const {
    return const_cast<atlas*>(this)->entry(id);
}

bool lx::gfx::atlas::create_page(unsigned page) {
    page_state& p = pages_[page];
    if (p.size != 0)
        return true;
    const unsigned bpt = bytes_per_texel(page_format(page));
    const std::size_t bytes = static_cast<std::size_t>(k_atlas_page_max) * k_atlas_page_max * bpt;
    // calloc: zero texels, and a large calloc maps lazily — the untouched 3/4 of a page
    // that has not grown costs address space, not memory.
    p.pixels = static_cast<std::uint8_t*>(std::calloc(bytes, 1));
    if (!p.pixels)
        return false;
    if (scratch_bytes_ < bytes) {
        std::free(scratch_);
        scratch_ = static_cast<std::uint8_t*>(std::calloc(bytes, 1));
        scratch_bytes_ = scratch_ ? bytes : 0;
    }
    p.stride = k_atlas_page_max * bpt;
    p.size = k_atlas_page_initial;
    p.next_y = 0;
    p.shelf_count = 0;
    p.dirty_count = 0;
    p.dirty_full = false;
    p.live = 0;
    p.live_area = 0;
    ++stats_.pages;
    return true;
}

bool lx::gfx::atlas::grow(unsigned page) {
    page_state& p = pages_[page];
    if (p.size == 0 || p.size >= k_atlas_page_max)
        return false;
    p.size = k_atlas_page_max;
    // A GPU copy of the page has to be recreated at the new size: upload it whole.
    p.dirty_full = true;
    p.dirty_count = 0;
    ++stats_.grows;
    return true;
}

bool lx::gfx::atlas::place(unsigned page, unsigned w, unsigned h, unsigned& shelf_out,
                           unsigned& x_out, unsigned& y_out) {
    page_state& p = pages_[page];
    if (p.size == 0 || w > p.size || h > p.size)
        return false;
    const unsigned bucket = bucket_height(h) < p.size ? bucket_height(h) : p.size;
    // 1. A shelf of this bucket with room at its end.
    for (unsigned s = 0; s < p.shelf_count; ++s) {
        shelf& sh = p.shelves[s];
        if (sh.height == bucket && sh.tail + w <= p.size) {
            shelf_out = s;
            x_out = sh.tail;
            y_out = sh.y;
            sh.tail = static_cast<std::uint16_t>(sh.tail + w);
            ++sh.live;
            return true;
        }
    }
    // 2. A new shelf below the last.
    if (p.next_y + bucket <= p.size && p.shelf_count < k_atlas_max_shelves) {
        shelf& sh = p.shelves[p.shelf_count];
        sh = {};
        sh.y = static_cast<std::uint16_t>(p.next_y);
        sh.height = static_cast<std::uint16_t>(bucket);
        sh.tail = static_cast<std::uint16_t>(w);
        sh.live = 1;
        shelf_out = p.shelf_count++;
        x_out = 0;
        y_out = sh.y;
        p.next_y += bucket;
        return true;
    }
    // 3. An emptied shelf at least as tall (the lowest such), taken over by this bucket.
    unsigned best = k_atlas_max_shelves;
    for (unsigned s = 0; s < p.shelf_count; ++s) {
        const shelf& sh = p.shelves[s];
        if (sh.live == 0 && sh.height >= h && (best == k_atlas_max_shelves ||
                                               sh.height < p.shelves[best].height))
            best = s;
    }
    if (best != k_atlas_max_shelves) {
        shelf& sh = p.shelves[best];
        sh.tail = static_cast<std::uint16_t>(w);
        sh.live = 1;
        shelf_out = best;
        x_out = 0;
        y_out = sh.y;
        return true;
    }
    return false;
}

void lx::gfx::atlas::free_tile(unsigned index) {
    tile_entry& t = store_->tiles[index];
    if (!t.live)
        return;
    page_state& p = pages_[t.page];
    shelf& sh = p.shelves[t.shelf];
    if (sh.live > 0)
        --sh.live;
    if (sh.live == 0)
        sh.tail = 0;
    else if (static_cast<unsigned>(t.x) + t.w == sh.tail)
        sh.tail = t.x; // the last tile of its shelf: the space is reusable at once
    if (sh.live == 0 && t.shelf + 1u == p.shelf_count) {
        // The bottom shelf emptied: give its rows back (and any emptied shelves above it).
        while (p.shelf_count > 0 && p.shelves[p.shelf_count - 1u].live == 0) {
            --p.shelf_count;
            p.next_y = p.shelves[p.shelf_count].y;
        }
    }
    --p.live;
    p.live_area -= static_cast<unsigned long long>(t.w) * t.h;
    t.live = false;
    t.generation = static_cast<std::uint16_t>(t.generation + 1u == 0 ? 1u : t.generation + 1u);
    store_->free_list[free_top_++] = static_cast<std::uint16_t>(index);
    --stats_.live_tiles;
}

lx::result<lx::gfx::atlas_tile_id> lx::gfx::atlas::allocate(atlas_format format, unsigned w,
                                                            unsigned h) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (w == 0 || h == 0 || w > k_atlas_page_max || h > k_atlas_page_max)
        return lx::invalid_argument("atlas::allocate: empty tile or larger than a page");
    if (free_top_ == 0)
        return lx::invalid_argument("atlas::allocate: tile table full");

    const unsigned first = format == atlas_format::r8 ? 0u : k_atlas_first_rgba8_page;
    const unsigned last = format == atlas_format::r8 ? k_atlas_r8_pages : k_atlas_pages;
    unsigned page = k_atlas_pages;
    unsigned sh = 0;
    unsigned x = 0;
    unsigned y = 0;
    // Existing pages as they are, then grown, then a new page, then reclaim and retry.
    for (unsigned attempt = 0; attempt < 2 && page == k_atlas_pages; ++attempt) {
        for (unsigned p = first; p < last && page == k_atlas_pages; ++p)
            if (place(p, w, h, sh, x, y))
                page = p;
        for (unsigned p = first; p < last && page == k_atlas_pages; ++p)
            if (pages_[p].size != 0 && grow(p) && place(p, w, h, sh, x, y))
                page = p;
        for (unsigned p = first; p < last && page == k_atlas_pages; ++p) {
            if (pages_[p].size != 0)
                continue;
            if (!create_page(p))
                return lx::invalid_argument("atlas::allocate: out of memory for a page");
            if (place(p, w, h, sh, x, y) || (grow(p) && place(p, w, h, sh, x, y)))
                page = p;
            break;
        }
        if (page == k_atlas_pages && (attempt > 0 || !reclaim(format)))
            break;
    }
    if (page == k_atlas_pages)
        return lx::invalid_argument("atlas::allocate: every page is full of live tiles");

    const unsigned index = store_->free_list[--free_top_];
    tile_entry& t = store_->tiles[index];
    t.page = static_cast<std::uint8_t>(page);
    t.shelf = static_cast<std::uint16_t>(sh);
    t.x = static_cast<std::uint16_t>(x);
    t.y = static_cast<std::uint16_t>(y);
    t.w = static_cast<std::uint16_t>(w);
    t.h = static_cast<std::uint16_t>(h);
    t.last_used = frame_;
    t.live = true;
    pages_[page].live += 1;
    pages_[page].live_area += static_cast<unsigned long long>(w) * h;
    ++stats_.live_tiles;
    return atlas_tile_id{(static_cast<std::uint32_t>(t.generation) << 16u) | (index + 1u)};
}

lx::result<void> lx::gfx::atlas::write(atlas_tile_id id, std::span<const std::uint8_t> pixels,
                                       unsigned stride) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    tile_entry* t = entry(id);
    if (!t)
        return lx::invalid_argument("atlas::write: stale or invalid tile");
    page_state& p = pages_[t->page];
    const unsigned bpt = bytes_per_texel(page_format(t->page));
    const std::size_t row_bytes = static_cast<std::size_t>(t->w) * bpt;
    if (stride < row_bytes ||
        pixels.size() < static_cast<std::size_t>(stride) * (t->h - 1u) + row_bytes)
        return lx::invalid_argument("atlas::write: pixel span smaller than the tile");
    for (unsigned row = 0; row < t->h; ++row)
        std::memcpy(p.pixels + static_cast<std::size_t>(t->y + row) * p.stride +
                        static_cast<std::size_t>(t->x) * bpt,
                    pixels.data() + static_cast<std::size_t>(row) * stride, row_bytes);
    add_dirty(t->page, {t->x, t->y, t->w, t->h});
    return {};
}

lx::result<lx::gfx::atlas_tile_id> lx::gfx::atlas::insert(atlas_format format, unsigned w,
                                                          unsigned h,
                                                          std::span<const std::uint8_t> pixels,
                                                          unsigned stride) {
    auto id = allocate(format, w, h);
    if (!id)
        return id;
    if (auto ok = write(*id, pixels, stride); !ok) {
        release(*id);
        return ok.get_error();
    }
    return id;
}

bool lx::gfx::atlas::lookup(atlas_tile_id id, atlas_tile& out) const {
    const tile_entry* t = entry(id);
    if (!t)
        return false;
    out = {t->page, t->x, t->y, t->w, t->h};
    return true;
}

void lx::gfx::atlas::touch(atlas_tile_id id) {
    if (tile_entry* t = entry(id))
        t->last_used = frame_;
}

void lx::gfx::atlas::release(atlas_tile_id id) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (const tile_entry* t = entry(id))
        free_tile(static_cast<unsigned>(t - store_->tiles));
}

unsigned lx::gfx::atlas::evict_stale() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    unsigned n = 0;
    for (unsigned i = 0; i < k_atlas_max_tiles; ++i) {
        const tile_entry& t = store_->tiles[i];
        if (t.live && stale(t)) {
            free_tile(i);
            ++n;
        }
    }
    stats_.evicted += n;
    return n;
}

bool lx::gfx::atlas::reclaim(atlas_format format) {
    const unsigned first = format == atlas_format::r8 ? 0u : k_atlas_first_rgba8_page;
    const unsigned last = format == atlas_format::r8 ? k_atlas_r8_pages : k_atlas_pages;
    bool freed = false;
    for (unsigned p = first; p < last; ++p) {
        if (pages_[p].size == 0 || stale_tiles(p) == 0)
            continue;
        // In place only: live tiles never move inside `allocate` (uvs of this frame may
        // already be in the stream). A crowded page is repacked at the next `begin_frame`.
        if (occupancy(p) > k_atlas_rebuild_occupancy)
            pages_[p].rebuild_pending = true;
        for (unsigned i = 0; i < k_atlas_max_tiles; ++i) {
            const tile_entry& t = store_->tiles[i];
            if (t.live && t.page == p && stale(t)) {
                free_tile(i);
                ++stats_.evicted;
                freed = true;
            }
        }
    }
    return freed;
}

lx::result<lx::gfx::atlas_rebuild_stats> lx::gfx::atlas::rebuild(unsigned page) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (page >= k_atlas_pages || pages_[page].size == 0)
        return lx::invalid_argument("atlas::rebuild: page not created");
    if (!scratch_)
        return lx::invalid_argument("atlas::rebuild: no scratch (allocation failed)");
    const std::uint64_t t0 = lx_gfx_atlas_now_ns();
    page_state& p = pages_[page];
    p.rebuild_pending = false;
    atlas_rebuild_stats out{};
    out.page = page;
    out.occupancy_before = occupancy(page);

    // 1. Stale tiles go; the live ones are gathered.
    unsigned count = 0;
    for (unsigned i = 0; i < k_atlas_max_tiles; ++i) {
        const tile_entry& t = store_->tiles[i];
        if (!t.live || t.page != page)
            continue;
        if (stale(t)) {
            free_tile(i);
            ++out.evicted;
            continue;
        }
        store_->order[count++] = static_cast<std::uint16_t>(i);
    }
    stats_.evicted += out.evicted;

    // 2. Tallest bucket first, then widest, then index (deterministic): shelves fill in
    //    descending height, which is what packs densest.
    tile_entry* tiles = store_->tiles;
    std::sort(store_->order, store_->order + count, [&](std::uint16_t a, std::uint16_t b) {
        const unsigned ba = bucket_height(tiles[a].h);
        const unsigned bb = bucket_height(tiles[b].h);
        if (ba != bb)
            return ba > bb;
        if (tiles[a].w != tiles[b].w)
            return tiles[a].w > tiles[b].w;
        return a < b;
    });

    // 3. Pixels out to the scratch, in repack order.
    const unsigned bpt = bytes_per_texel(page_format(page));
    std::size_t offset = 0;
    for (unsigned n = 0; n < count; ++n) {
        const tile_entry& t = tiles[store_->order[n]];
        const std::size_t row_bytes = static_cast<std::size_t>(t.w) * bpt;
        store_->scratch_offset[n] = static_cast<std::uint32_t>(offset);
        for (unsigned row = 0; row < t.h; ++row) {
            std::memcpy(scratch_ + offset,
                        p.pixels + static_cast<std::size_t>(t.y + row) * p.stride +
                            static_cast<std::size_t>(t.x) * bpt,
                        row_bytes);
            offset += row_bytes;
        }
    }

    // 4. Empty shelves, re-place, pixels back.
    p.shelf_count = 0;
    p.next_y = 0;
    for (unsigned n = 0; n < count; ++n) {
        const unsigned index = store_->order[n];
        tile_entry& t = tiles[index];
        unsigned sh = 0;
        unsigned x = 0;
        unsigned y = 0;
        if (!place(page, t.w, t.h, sh, x, y) && !(grow(page) && place(page, t.w, t.h, sh, x, y))) {
            // Cannot happen for a set that fitted before; released rather than overlapped.
            t.shelf = 0;
            --p.live;
            p.live_area -= static_cast<unsigned long long>(t.w) * t.h;
            t.live = false;
            t.generation = static_cast<std::uint16_t>(t.generation + 1u == 0 ? 1u
                                                                             : t.generation + 1u);
            store_->free_list[free_top_++] = static_cast<std::uint16_t>(index);
            --stats_.live_tiles;
            ++out.lost;
            continue;
        }
        t.shelf = static_cast<std::uint16_t>(sh);
        t.x = static_cast<std::uint16_t>(x);
        t.y = static_cast<std::uint16_t>(y);
        const std::size_t row_bytes = static_cast<std::size_t>(t.w) * bpt;
        const std::uint8_t* src = scratch_ + store_->scratch_offset[n];
        for (unsigned row = 0; row < t.h; ++row)
            std::memcpy(p.pixels + static_cast<std::size_t>(t.y + row) * p.stride +
                            static_cast<std::size_t>(t.x) * bpt,
                        src + static_cast<std::size_t>(row) * row_bytes, row_bytes);
        ++out.moved;
    }
    p.dirty_full = true;
    p.dirty_count = 0;

    out.occupancy_after = occupancy(page);
    out.ns = lx_gfx_atlas_now_ns() - t0;
    ++stats_.rebuilds;
    stats_.rebuild_lost += out.lost;
    stats_.last_rebuild_ns = out.ns;
    if (out.ns > stats_.max_rebuild_ns)
        stats_.max_rebuild_ns = out.ns;
    return out;
}

void lx::gfx::atlas::add_dirty(unsigned page, lx::rect2i r) {
    page_state& p = pages_[page];
    if (p.dirty_full || r.width <= 0 || r.height <= 0)
        return;
    // Merge with any rect whose bounding box with `r` wastes at most 25 % over the two
    // areas (adjacent tiles on a shelf merge into one strip); a merge can enable another,
    // so scan again until nothing merges.
    for (bool merged = true; merged;) {
        merged = false;
        for (unsigned i = p.dirty_count; i-- > 0;) {
            const lx::rect2i e = p.dirty[i];
            const lx::rect2i box = lx_gfx_atlas_bbox(e, r);
            if (lx_gfx_atlas_area(box) * 4 <= (lx_gfx_atlas_area(e) + lx_gfx_atlas_area(r)) * 5) {
                r = box;
                p.dirty[i] = p.dirty[--p.dirty_count];
                merged = true;
                break;
            }
        }
    }
    if (p.dirty_count >= k_atlas_dirty_rects) {
        p.dirty_full = true;
        p.dirty_count = 0;
        return;
    }
    p.dirty[p.dirty_count++] = r;
}

lx::gfx::atlas_page_view lx::gfx::atlas::page(unsigned page) const {
    if (page >= k_atlas_pages || pages_[page].size == 0)
        return {};
    const page_state& p = pages_[page];
    return {p.pixels, p.stride, p.size, page_format(page)};
}

float lx::gfx::atlas::occupancy(unsigned page) const {
    if (page >= k_atlas_pages || pages_[page].size == 0)
        return 0.f;
    const page_state& p = pages_[page];
    unsigned long long claimed = 0;
    for (unsigned s = 0; s < p.shelf_count; ++s)
        claimed += static_cast<unsigned long long>(p.shelves[s].tail) * p.shelves[s].height;
    return static_cast<float>(static_cast<double>(claimed) /
                              (static_cast<double>(p.size) * p.size));
}

float lx::gfx::atlas::density(unsigned page) const {
    if (page >= k_atlas_pages || pages_[page].size == 0)
        return 0.f;
    const page_state& p = pages_[page];
    return static_cast<float>(static_cast<double>(p.live_area) /
                              (static_cast<double>(p.size) * p.size));
}

unsigned lx::gfx::atlas::live_tiles(unsigned page) const {
    return page < k_atlas_pages ? pages_[page].live : 0u;
}

unsigned lx::gfx::atlas::stale_tiles(unsigned page) const {
    unsigned n = 0;
    for (unsigned i = 0; i < k_atlas_max_tiles; ++i) {
        const tile_entry& t = store_->tiles[i];
        if (t.live && t.page == page && stale(t))
            ++n;
    }
    return n;
}

std::span<const lx::rect2i> lx::gfx::atlas::dirty_rects(unsigned page) const {
    if (page >= k_atlas_pages || pages_[page].dirty_full)
        return {};
    return {pages_[page].dirty, pages_[page].dirty_count};
}

bool lx::gfx::atlas::dirty_full(unsigned page) const {
    return page < k_atlas_pages && pages_[page].dirty_full;
}

void lx::gfx::atlas::clear_dirty(unsigned page) {
    if (page >= k_atlas_pages)
        return;
    pages_[page].dirty_count = 0;
    pages_[page].dirty_full = false;
}

lx::gfx::atlas_stats lx::gfx::atlas::stats() const { return stats_; }
