module;

#if defined(LUMEN_HAS_TEXT)
#include <hb.h>
#endif

import lx.foundation;
import lx.runtime;
import lx.trace;

export module lx.text:shape;

import std;

import :faces;

namespace {

constexpr lx::trace::category log_text{"text"};

} // namespace

export namespace lx::text {

// ── Shaping and the shape cache (ADR-010 §4.6 `:shape`) ──────────────────────────
//
// `shape_cache` shapes one run — one face, size, direction, script, language and feature
// set over a stretch of UTF-8 — through HarfBuzz into glyph ids, advances, offsets and
// clusters, and keeps the result: `k_shape_cache_entries` entries keyed by (face id, size
// in 26.6, features + language hash, direction, script, FNV-1a 64 of the UTF-8, its byte
// length), least recently used out. The hash only finds the entry: a hit also compares the
// run's UTF-8 with the bytes the entry keeps, so two runs whose hashes collide (FNV-1a is
// easily inverted: a client could craft a window title that collides with another's) never
// share glyphs. The ellipsis runs `text_layout` shapes per face go through the same check.
//
// Glyphs live in one arena of `glyph_capacity` records and the runs' UTF-8 in another of
// `text_capacity` bytes, both mapped at construction (anonymous pages, resident as runs
// fill them, handed back by `clear`); nothing allocates after construction except
// HarfBuzz itself while warming up (a shape plan per face and feature
// set, a buffer that grows past its preallocation for a run longer than `max_run_glyphs`
// code points). A full table or arena applies the cache's `overflow_action`: `drop_oldest`
// (the default) evicts least recently used entries — down to 1/16 of each arena free, so the
// arenas are compacted (live runs slid down, in arena order) once per that many glyphs or
// bytes rather than on every miss; `drop_newest`, `reject` and
// `warn_and_drop` (one warning per cache) keep the cache as it is and return the new run
// from a scratch run that is not stored. A run over `max_run_glyphs` glyphs is cut at the
// last whole cluster that fits and flagged `truncated`.
//
// The counters are cumulative and cheap to read: a frame that reads `shaping_calls` before
// and after its work sees how many runs it shaped (G4: none after warm-up).
//
// Threading contract. A shape cache is not locked: it has one owner at a time — the UI
// thread's cache, a worker's own cache (results posted to UI as data). `set_owner_affinity`
// makes `shape` assert it. A miss shapes under the face cache's lock (`shape` takes it,
// `shape_locked` expects the caller to hold it); a hit takes no lock. `shaped_run::glyphs`
// points into the cache and stays valid until the next `shape*` call on the same cache.
// `shared()`, the cache behind the single-line API (`shape_line`, `font_stack`), is the
// exception to single ownership: it is used only under `face_cache::shared().lock()`.

enum class text_direction : std::uint8_t {
    /// From the text (HarfBuzz's guess: the first strong character's script).
    automatic,
    ltr,
    rtl,
    ttb,
    btt,
};

[[nodiscard]] constexpr std::uint32_t make_tag(char a, char b, char c, char d) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}
/// `make_tag("liga")`, `make_tag("Arab")` (an ISO 15924 script tag is HarfBuzz's hb_script_t).
[[nodiscard]] constexpr std::uint32_t make_tag(const char (&s)[5]) {
    return make_tag(s[0], s[1], s[2], s[3]);
}

/// An OpenType feature over the run's clusters [start, end) (all of it by default).
struct font_feature {
    std::uint32_t tag = 0;
    std::uint32_t value = 1;
    std::uint32_t start = 0;
    std::uint32_t end = 0xFFFFFFFFu;
};

inline constexpr unsigned k_max_shape_features = 32;

struct shape_request {
    face_id face{};
    /// Pixels per em × scale, 26.6 (`size_26_6_from_pt`).
    std::int32_t size_26_6 = 0;
    std::string_view utf8{};
    text_direction direction = text_direction::automatic;
    /// ISO 15924 tag (`make_tag("Cyrl")`); 0 = from the text.
    std::uint32_t script = 0;
    /// BCP 47 language; null = the process locale's.
    const char* language = nullptr;
    std::span<const font_feature> features{};
};

/// One glyph, 26.6 pixels. `cluster` is the byte offset of its cluster in the run's UTF-8.
struct shaped_glyph {
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0;
    std::int32_t x_advance = 0;
    std::int32_t y_advance = 0;
    std::int32_t x_offset = 0;
    std::int32_t y_offset = 0;
};
static_assert(sizeof(shaped_glyph) == 24);

struct shaped_run {
    /// Visual order. Valid until the next `shape*` call on the cache that returned it.
    std::span<const shaped_glyph> glyphs{};
    /// Sum of the advances, 26.6.
    std::int32_t x_advance = 0;
    std::int32_t y_advance = 0;
    /// As shaped (resolved from the text when the request said `automatic` / 0).
    text_direction direction = text_direction::ltr;
    std::uint32_t script = 0;
    /// Answered from the cache, no shaping.
    bool hit = false;
    /// In the cache (a hit, or a miss that was stored); false: the overflow action kept it out.
    bool stored = false;
    /// Cut at `max_run_glyphs`.
    bool truncated = false;
};

inline constexpr unsigned k_shape_cache_entries = 4096;
/// 32 glyphs per entry on average; 3 MiB of address space, resident as it fills.
inline constexpr unsigned k_shape_cache_glyphs = 131072;
inline constexpr unsigned k_shape_max_run_glyphs = 4096;

struct shape_cache_config {
    unsigned entries = k_shape_cache_entries;
    unsigned glyph_capacity = k_shape_cache_glyphs;
    /// Longer runs are cut; at most `glyph_capacity`.
    unsigned max_run_glyphs = k_shape_max_run_glyphs;
    lx::runtime::overflow_action overflow = lx::runtime::overflow_action::drop_oldest;
    /// UTF-8 bytes of the stored runs (compared on every hit); 0 is 4 × `glyph_capacity`.
    unsigned text_capacity = 0;
};

struct shape_cache_counters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    /// hb_shape calls. Equal to `misses` except for a miss that failed before shaping.
    std::uint64_t shaping_calls = 0;
    std::uint64_t evictions = 0;
    /// Misses the overflow action kept out of the cache.
    std::uint64_t overflows = 0;
    std::uint64_t compactions = 0;
    unsigned entries = 0;
    unsigned entry_capacity = 0;
    /// Live glyph records × their size.
    std::uint64_t bytes_in_use = 0;
    /// The glyph arena.
    std::uint64_t bytes_capacity = 0;
    /// The stored runs' UTF-8, and its arena.
    std::uint64_t text_bytes_in_use = 0;
    std::uint64_t text_bytes_capacity = 0;
};

[[nodiscard]] constexpr std::uint64_t fnv1a_64(std::string_view bytes) {
    std::uint64_t h = 14695981039346656037ull;
    for (char c : bytes)
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

class shape_cache {
public:
    /// The cache behind the single-line API, guarded by `face_cache::shared().lock()`.
    [[nodiscard]] static shape_cache& shared();

    explicit shape_cache(shape_cache_config config = {});
    ~shape_cache();

    shape_cache(const shape_cache&) = delete;
    shape_cache& operator=(const shape_cache&) = delete;

    /// The run from the cache, or shaped (under `faces.lock()`) and stored. Empty text is an
    /// empty run (not counted). Errors: `invalid_argument` (no size, too many features),
    /// `io` (the face id is stale, or no shaping buffer or storage), `not_implemented` without
    /// HarfBuzz.
    [[nodiscard]] lx::result<shaped_run> shape(face_cache& faces, const shape_request& request);
    /// As `shape`, with `faces.lock()` already held by the caller.
    [[nodiscard]] lx::result<shaped_run> shape_locked(face_cache& faces,
                                                      const shape_request& request);

    [[nodiscard]] shape_cache_counters counters() const;
    /// Drops every entry (counters stay).
    void clear();

    /// `shape` asserts this affinity (`any`, the default, checks nothing).
    void set_owner_affinity(lx::runtime::affinity owner) { owner_ = owner; }

private:
    struct entry;
    struct key {
        std::uint64_t hash = 0;
        std::uint64_t text_hash = 0;
        std::uint64_t features_hash = 0;
        std::uint32_t face = 0;
        std::int32_t size = 0;
        std::uint32_t bytes = 0;
        std::uint32_t script = 0;
        std::uint8_t direction = 0;
    };

    [[nodiscard]] lx::result<key> make_key(const shape_request& r) const;
    /// The entry for `k` whose kept bytes equal `utf8`, or `k_none`.
    [[nodiscard]] std::uint32_t find(const key& k, std::string_view utf8) const;
    [[nodiscard]] shaped_run hit(std::uint32_t index);
    [[nodiscard]] lx::result<shaped_run> miss_locked(face_cache& faces, const shape_request& r,
                                                     const key& k);
    void touch(std::uint32_t index);
    void evict(std::uint32_t index);
    void index_insert(std::uint32_t index);
    void index_remove(std::uint32_t index);
    [[nodiscard]] bool take_entry(std::uint32_t& index);
    /// Room for `n` glyphs and `bytes` of UTF-8 (evicting, compacting); false: none.
    [[nodiscard]] bool reserve(std::uint32_t n, std::uint32_t bytes, std::uint32_t& offset,
                               std::uint32_t& text_offset);
    void compact();

    shape_cache_config config_{};
    entry* entries_ = nullptr;
    std::uint32_t* index_ = nullptr; ///< entry + 1, 0 = empty; linear probing
    std::uint32_t index_mask_ = 0;
    shaped_glyph* arena_ = nullptr;
    shaped_glyph* scratch_ = nullptr;
    char* text_arena_ = nullptr;
    /// Backing for the three above: anonymous mappings, resident only as runs fill them.
    lx::runtime::page_mapping arena_memory_{};
    lx::runtime::page_mapping scratch_memory_{};
    lx::runtime::page_mapping text_memory_{};
    std::uint32_t text_top_ = 0;
    std::uint32_t live_text_ = 0;
    void* buffer_ = nullptr; ///< hb_buffer_t*
    std::uint32_t free_head_ = 0;
    std::uint32_t lru_head_ = 0; ///< most recently used
    std::uint32_t lru_tail_ = 0;
    std::uint32_t arena_head_ = 0; ///< lowest offset
    std::uint32_t arena_tail_ = 0;
    std::uint32_t top_ = 0;
    std::uint32_t live_glyphs_ = 0;
    std::uint32_t live_entries_ = 0;
    shape_cache_counters counters_{};
    lx::runtime::affinity owner_ = lx::runtime::affinity::any;
    bool warned_ = false;
};

} // namespace lx::text

// ── Implementation ────────────────────────────────────────────────────────────

namespace {

constexpr std::uint32_t k_none = 0xFFFFFFFFu;

constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

constexpr std::uint64_t finalize(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

struct lx::text::shape_cache::entry {
    key k{};
    std::uint32_t script_out = 0;
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    /// The run's UTF-8 (`k.bytes` of it) in the text arena.
    std::uint32_t text_offset = 0;
    std::int32_t x_advance = 0;
    std::int32_t y_advance = 0;
    std::uint32_t lru_prev = k_none;
    std::uint32_t lru_next = k_none;
    std::uint32_t arena_prev = k_none;
    std::uint32_t arena_next = k_none;
    std::uint8_t direction_out = 0;
    bool live = false;
    bool truncated = false;
};

lx::text::shape_cache& lx::text::shape_cache::shared() {
    static shape_cache c{};
    return c;
}

lx::text::shape_cache::shape_cache(shape_cache_config config) : config_{config} {
    if (config_.entries == 0)
        config_.entries = 1;
    if (config_.glyph_capacity == 0)
        config_.glyph_capacity = 1;
    if (config_.max_run_glyphs == 0 || config_.max_run_glyphs > config_.glyph_capacity)
        config_.max_run_glyphs = config_.glyph_capacity;
    if (config_.text_capacity == 0)
        config_.text_capacity = config_.glyph_capacity <= 0x3FFFFFFFu
                                    ? config_.glyph_capacity * 4u
                                    : 0xFFFFFFFFu;
    unsigned slots = 1;
    while (slots < config_.entries * 2)
        slots <<= 1;
    entries_ = new (std::nothrow) entry[config_.entries];
    index_ = new (std::nothrow) std::uint32_t[slots]{};
    index_mask_ = slots - 1;
    // Anonymous mappings, no constructor run (shaped_glyph is an implicit-lifetime
    // aggregate): an arena's pages become resident only as runs fill them. Heap blocks would
    // not stay lazy — see lx::runtime::page_mapping.
    if (arena_memory_.map(std::size_t{config_.glyph_capacity} * sizeof(shaped_glyph)))
        arena_ = reinterpret_cast<shaped_glyph*>(arena_memory_.data());
    if (scratch_memory_.map(std::size_t{config_.max_run_glyphs} * sizeof(shaped_glyph)))
        scratch_ = reinterpret_cast<shaped_glyph*>(scratch_memory_.data());
    if (text_memory_.map(std::size_t{config_.text_capacity}))
        text_arena_ = reinterpret_cast<char*>(text_memory_.data());
    if (!entries_ || !index_ || !arena_ || !scratch_ || !text_arena_) {
        // Out of memory at construction: every shape call reports it.
        delete[] entries_;
        delete[] index_;
        arena_memory_.release();
        scratch_memory_.release();
        text_memory_.release();
        entries_ = nullptr;
        index_ = nullptr;
        arena_ = scratch_ = nullptr;
        text_arena_ = nullptr;
        config_.entries = 0;
        config_.glyph_capacity = 0;
        config_.max_run_glyphs = 0;
        config_.text_capacity = 0;
    }
    for (std::uint32_t i = 0; i < config_.entries; ++i)
        entries_[i].lru_next = i + 1 < config_.entries ? i + 1 : k_none;
    free_head_ = config_.entries ? 0 : k_none;
    lru_head_ = lru_tail_ = arena_head_ = arena_tail_ = k_none;
    counters_.entry_capacity = config_.entries;
    counters_.bytes_capacity = static_cast<std::uint64_t>(config_.glyph_capacity) * sizeof(shaped_glyph);
    counters_.text_bytes_capacity = config_.text_capacity;
#if defined(LUMEN_HAS_TEXT)
    hb_buffer_t* buf = hb_buffer_create();
    if (buf && hb_buffer_allocation_successful(buf)) {
        (void)hb_buffer_pre_allocate(buf, config_.max_run_glyphs);
        buffer_ = buf;
    } else if (buf) {
        hb_buffer_destroy(buf);
    }
#endif
}

lx::text::shape_cache::~shape_cache() {
#if defined(LUMEN_HAS_TEXT)
    if (buffer_)
        hb_buffer_destroy(static_cast<hb_buffer_t*>(buffer_));
#endif
    delete[] entries_;
    delete[] index_;
}

lx::text::shape_cache_counters lx::text::shape_cache::counters() const {
    shape_cache_counters c = counters_;
    c.entries = live_entries_;
    c.bytes_in_use = static_cast<std::uint64_t>(live_glyphs_) * sizeof(shaped_glyph);
    c.text_bytes_in_use = live_text_;
    return c;
}

void lx::text::shape_cache::clear() {
    while (lru_tail_ != k_none) {
        evict(lru_tail_);
        --counters_.evictions; // a clear is not pressure
    }
    top_ = 0;
    text_top_ = 0;
    // Nothing is stored: the arenas' pages go back to the kernel.
    arena_memory_.discard(0, arena_memory_.size());
    text_memory_.discard(0, text_memory_.size());
}

lx::result<lx::text::shape_cache::key> lx::text::shape_cache::make_key(const shape_request& r) const {
    if (r.size_26_6 <= 0)
        return lx::invalid_argument("shape: size_26_6 must be positive");
    if (r.features.size() > k_max_shape_features)
        return lx::invalid_argument("shape: too many features");
    if (r.utf8.size() > 0xFFFFFFFFu)
        return lx::invalid_argument("shape: run too long");
    key k{};
    k.text_hash = fnv1a_64(r.utf8);
    k.bytes = static_cast<std::uint32_t>(r.utf8.size());
    std::uint64_t fh = 14695981039346656037ull;
    const auto feed = [&fh](std::uint32_t v) {
        for (int b = 0; b < 4; ++b)
            fh = (fh ^ ((v >> (8 * b)) & 0xFFu)) * 1099511628211ull;
    };
    for (const font_feature& f : r.features) {
        feed(f.tag);
        feed(f.value);
        feed(f.start);
        feed(f.end);
    }
    if (r.language) {
        feed(0x4C414E47u); // separates the language from the features
        for (const char* p = r.language; *p; ++p)
            fh = (fh ^ static_cast<unsigned char>(*p)) * 1099511628211ull;
    }
    k.features_hash = fh;
    k.face = r.face.value;
    k.size = r.size_26_6;
    k.script = r.script;
    k.direction = static_cast<std::uint8_t>(r.direction);
    std::uint64_t h = k.text_hash;
    h = mix(h, k.features_hash);
    h = mix(h, (static_cast<std::uint64_t>(k.face) << 32) | static_cast<std::uint32_t>(k.size));
    h = mix(h, (static_cast<std::uint64_t>(k.script) << 32) | k.bytes);
    h = mix(h, k.direction);
    k.hash = finalize(h);
    return k;
}

std::uint32_t lx::text::shape_cache::find(const key& k, std::string_view utf8) const {
    for (std::uint32_t i = static_cast<std::uint32_t>(k.hash) & index_mask_;; i = (i + 1) & index_mask_) {
        const std::uint32_t slot = index_[i];
        if (slot == 0)
            return k_none;
        const entry& en = entries_[slot - 1];
        const key& e = en.k;
        // The hash finds the entry; the bytes decide it (a colliding run is a miss).
        if (e.hash == k.hash && e.text_hash == k.text_hash && e.features_hash == k.features_hash &&
            e.face == k.face && e.size == k.size && e.bytes == k.bytes && e.script == k.script &&
            e.direction == k.direction &&
            std::memcmp(text_arena_ + en.text_offset, utf8.data(), k.bytes) == 0)
            return slot - 1;
    }
}

void lx::text::shape_cache::touch(std::uint32_t i) {
    if (lru_head_ == i)
        return;
    entry& e = entries_[i];
    // Unlink…
    if (e.lru_prev != k_none)
        entries_[e.lru_prev].lru_next = e.lru_next;
    if (e.lru_next != k_none)
        entries_[e.lru_next].lru_prev = e.lru_prev;
    if (lru_tail_ == i)
        lru_tail_ = e.lru_prev;
    // …and put first.
    e.lru_prev = k_none;
    e.lru_next = lru_head_;
    if (lru_head_ != k_none)
        entries_[lru_head_].lru_prev = i;
    lru_head_ = i;
    if (lru_tail_ == k_none)
        lru_tail_ = i;
}

void lx::text::shape_cache::index_insert(std::uint32_t index) {
    std::uint32_t i = static_cast<std::uint32_t>(entries_[index].k.hash) & index_mask_;
    while (index_[i] != 0)
        i = (i + 1) & index_mask_;
    index_[i] = index + 1;
}

void lx::text::shape_cache::index_remove(std::uint32_t index) {
    std::uint32_t i = static_cast<std::uint32_t>(entries_[index].k.hash) & index_mask_;
    while (index_[i] != index + 1) {
        if (index_[i] == 0)
            return; // not indexed
        i = (i + 1) & index_mask_;
    }
    // Backward-shift deletion: no tombstones, probe chains stay short.
    std::uint32_t j = i;
    while (true) {
        j = (j + 1) & index_mask_;
        if (index_[j] == 0)
            break;
        const std::uint32_t home =
            static_cast<std::uint32_t>(entries_[index_[j] - 1].k.hash) & index_mask_;
        // Leave j where it is when its home lies cyclically in (i, j].
        const bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
        if (stays)
            continue;
        index_[i] = index_[j];
        i = j;
    }
    index_[i] = 0;
}

void lx::text::shape_cache::evict(std::uint32_t i) {
    entry& e = entries_[i];
    index_remove(i);
    if (e.lru_prev != k_none)
        entries_[e.lru_prev].lru_next = e.lru_next;
    else
        lru_head_ = e.lru_next;
    if (e.lru_next != k_none)
        entries_[e.lru_next].lru_prev = e.lru_prev;
    else
        lru_tail_ = e.lru_prev;
    if (e.arena_prev != k_none)
        entries_[e.arena_prev].arena_next = e.arena_next;
    else
        arena_head_ = e.arena_next;
    if (e.arena_next != k_none)
        entries_[e.arena_next].arena_prev = e.arena_prev;
    else
        arena_tail_ = e.arena_prev;
    live_glyphs_ -= e.count;
    live_text_ -= e.k.bytes;
    --live_entries_;
    e = entry{};
    e.lru_next = free_head_;
    free_head_ = i;
    ++counters_.evictions;
    if (arena_tail_ == k_none)
        top_ = text_top_ = 0;
}

bool lx::text::shape_cache::take_entry(std::uint32_t& index) {
    if (free_head_ == k_none) {
        if (config_.overflow != lx::runtime::overflow_action::drop_oldest || lru_tail_ == k_none)
            return false;
        evict(lru_tail_);
    }
    index = free_head_;
    free_head_ = entries_[index].lru_next;
    return true;
}

void lx::text::shape_cache::compact() {
    // Both arenas fill in arena order (an entry's glyphs and bytes are reserved together).
    std::uint32_t dst = 0;
    std::uint32_t text_dst = 0;
    for (std::uint32_t i = arena_head_; i != k_none; i = entries_[i].arena_next) {
        entry& e = entries_[i];
        if (e.offset != dst && e.count > 0)
            std::memmove(arena_ + dst, arena_ + e.offset, e.count * sizeof(shaped_glyph));
        e.offset = dst;
        dst += e.count;
        if (e.text_offset != text_dst && e.k.bytes > 0)
            std::memmove(text_arena_ + text_dst, text_arena_ + e.text_offset, e.k.bytes);
        e.text_offset = text_dst;
        text_dst += e.k.bytes;
    }
    top_ = dst;
    text_top_ = text_dst;
    ++counters_.compactions;
}

bool lx::text::shape_cache::reserve(std::uint32_t n, std::uint32_t bytes, std::uint32_t& offset,
                                    std::uint32_t& text_offset) {
    const std::uint32_t cap = config_.glyph_capacity;
    const std::uint32_t text_cap = config_.text_capacity;
    if (n > cap || bytes > text_cap)
        return false;
    if (top_ + n > cap || text_top_ + bytes > text_cap) {
        if (config_.overflow == lx::runtime::overflow_action::drop_oldest) {
            // Free the room asked for plus 1/16 of each arena, so the compaction below runs
            // once per that many glyphs or bytes, not on every miss once an arena is full.
            const auto want = [](std::uint64_t asked, std::uint32_t capacity) {
                const std::uint64_t slack = capacity / 16;
                return asked + slack < capacity ? asked + slack : std::uint64_t{capacity};
            };
            const std::uint64_t want_glyphs = top_ + n > cap ? want(n, cap) : n;
            const std::uint64_t want_text = text_top_ + bytes > text_cap ? want(bytes, text_cap)
                                                                          : bytes;
            while (lru_tail_ != k_none &&
                   (static_cast<std::uint64_t>(cap - live_glyphs_) < want_glyphs ||
                    static_cast<std::uint64_t>(text_cap - live_text_) < want_text))
                evict(lru_tail_);
        }
        if (live_glyphs_ + n > cap || live_text_ + bytes > text_cap)
            return false;
        compact();
    }
    offset = top_;
    top_ += n;
    text_offset = text_top_;
    text_top_ += bytes;
    return true;
}

lx::text::shaped_run lx::text::shape_cache::hit(std::uint32_t i) {
    touch(i);
    const entry& e = entries_[i];
    ++counters_.hits;
    shaped_run r{};
    r.glyphs = {arena_ + e.offset, e.count};
    r.x_advance = e.x_advance;
    r.y_advance = e.y_advance;
    r.direction = static_cast<text_direction>(e.direction_out);
    r.script = e.script_out;
    r.hit = true;
    r.stored = true;
    r.truncated = e.truncated;
    return r;
}

lx::result<lx::text::shaped_run> lx::text::shape_cache::shape(face_cache& faces,
                                                              const shape_request& request) {
    lx::runtime::assert_affinity(owner_);
    if (request.utf8.empty())
        return shaped_run{};
    if (!index_)
        return lx::make_error(lx::io_err::not_found, "shape: no cache storage");
    const lx::result<key> k = make_key(request);
    if (!k)
        return k.get_error();
    if (const std::uint32_t i = find(*k, request.utf8); i != k_none)
        return hit(i);
    auto lock = faces.lock();
    return miss_locked(faces, request, *k);
}

lx::result<lx::text::shaped_run> lx::text::shape_cache::shape_locked(face_cache& faces,
                                                                     const shape_request& request) {
    lx::runtime::assert_affinity(owner_);
    if (request.utf8.empty())
        return shaped_run{};
    if (!index_)
        return lx::make_error(lx::io_err::not_found, "shape: no cache storage");
    const lx::result<key> k = make_key(request);
    if (!k)
        return k.get_error();
    if (const std::uint32_t i = find(*k, request.utf8); i != k_none)
        return hit(i);
    return miss_locked(faces, request, *k);
}

lx::result<lx::text::shaped_run> lx::text::shape_cache::miss_locked(face_cache& faces,
                                                                    const shape_request& r,
                                                                    const key& k) {
#if defined(LUMEN_HAS_TEXT)
    ++counters_.misses;
    auto* buf = static_cast<hb_buffer_t*>(buffer_);
    if (!buf)
        return lx::make_error(lx::io_err::not_found, "shape: no harfbuzz buffer");
    const face_cache::handles h = faces.use_locked(r.face, r.size_26_6);
    if (!h.hb_font)
        return lx::make_error(lx::io_err::not_found, "shape: face not loaded");

    hb_buffer_clear_contents(buf);
    hb_buffer_add_utf8(buf, r.utf8.data(), static_cast<int>(r.utf8.size()), 0,
                       static_cast<int>(r.utf8.size()));
    switch (r.direction) {
    case text_direction::ltr: hb_buffer_set_direction(buf, HB_DIRECTION_LTR); break;
    case text_direction::rtl: hb_buffer_set_direction(buf, HB_DIRECTION_RTL); break;
    case text_direction::ttb: hb_buffer_set_direction(buf, HB_DIRECTION_TTB); break;
    case text_direction::btt: hb_buffer_set_direction(buf, HB_DIRECTION_BTT); break;
    case text_direction::automatic: break;
    }
    if (r.script)
        hb_buffer_set_script(buf, static_cast<hb_script_t>(r.script));
    if (r.language)
        hb_buffer_set_language(buf, hb_language_from_string(r.language, -1));
    hb_buffer_guess_segment_properties(buf);
    hb_feature_t features[k_max_shape_features];
    unsigned nf = 0;
    for (const font_feature& f : r.features)
        features[nf++] = {static_cast<hb_tag_t>(f.tag), f.value, f.start, f.end};
    hb_shape(static_cast<hb_font_t*>(h.hb_font), buf, nf ? features : nullptr, nf);
    ++counters_.shaping_calls;

    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    if (!info || !pos)
        n = 0;
    std::uint32_t count = n < config_.max_run_glyphs ? n : config_.max_run_glyphs;
    const bool truncated = count < n;
    // Over the cap: drop the partial cluster at the cut too.
    if (truncated)
        while (count > 0 && info[count].cluster == info[count - 1].cluster)
            --count;

    std::uint32_t index = k_none;
    std::uint32_t offset = 0;
    std::uint32_t text_offset = 0;
    bool stored = false;
    if (take_entry(index)) {
        if (reserve(count, k.bytes, offset, text_offset)) {
            stored = true;
        } else {
            // Give the entry back: no room for its glyphs.
            entries_[index].lru_next = free_head_;
            free_head_ = index;
        }
    }
    shaped_glyph* out = stored ? arena_ + offset : scratch_;
    std::int64_t x = 0;
    std::int64_t y = 0;
    for (std::uint32_t g = 0; g < count; ++g) {
        out[g] = {info[g].codepoint, info[g].cluster, pos[g].x_advance, pos[g].y_advance,
                  pos[g].x_offset, pos[g].y_offset};
        x += pos[g].x_advance;
        y += pos[g].y_advance;
    }
    shaped_run run{};
    run.glyphs = {out, count};
    run.x_advance = static_cast<std::int32_t>(x);
    run.y_advance = static_cast<std::int32_t>(y);
    switch (hb_buffer_get_direction(buf)) {
    case HB_DIRECTION_RTL: run.direction = text_direction::rtl; break;
    case HB_DIRECTION_TTB: run.direction = text_direction::ttb; break;
    case HB_DIRECTION_BTT: run.direction = text_direction::btt; break;
    default: run.direction = text_direction::ltr; break;
    }
    run.script = static_cast<std::uint32_t>(hb_buffer_get_script(buf));
    run.truncated = truncated;
    run.stored = stored;
    if (!stored) {
        ++counters_.overflows;
        if (config_.overflow == lx::runtime::overflow_action::warn_and_drop && !warned_) {
            warned_ = true;
            log_text.warn("shape cache full: runs are shaped but not kept");
        }
        return run;
    }
    entry& e = entries_[index];
    e = entry{};
    e.k = k;
    e.script_out = run.script;
    e.offset = offset;
    e.count = count;
    e.text_offset = text_offset;
    if (k.bytes > 0)
        std::memcpy(text_arena_ + text_offset, r.utf8.data(), k.bytes);
    e.x_advance = run.x_advance;
    e.y_advance = run.y_advance;
    e.direction_out = static_cast<std::uint8_t>(run.direction);
    e.truncated = truncated;
    e.live = true;
    // Newest in arena order (highest offset) and most recently used.
    e.arena_prev = arena_tail_;
    if (arena_tail_ != k_none)
        entries_[arena_tail_].arena_next = index;
    else
        arena_head_ = index;
    arena_tail_ = index;
    e.lru_next = lru_head_;
    if (lru_head_ != k_none)
        entries_[lru_head_].lru_prev = index;
    lru_head_ = index;
    if (lru_tail_ == k_none)
        lru_tail_ = index;
    index_insert(index);
    live_glyphs_ += count;
    live_text_ += k.bytes;
    ++live_entries_;
    return run;
#else
    (void)faces;
    (void)r;
    (void)k;
    return lx::not_implemented("lx::text::shape_cache::shape");
#endif
}
