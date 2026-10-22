module;

#if defined(LUMEN_HAS_TEXT)
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>
#endif

import lx.foundation;
import lx.gfx;

export module lx.text;

import std;

export namespace lx::text {

enum class weight { normal, bold };
enum class style { normal, italic };

struct font_desc {
    const char* family = "Sans";
    float size_pt = 11.f;
    weight weight = weight::normal;
    style style = style::normal;
};

struct glyph_run {
    const char* utf8 = "";
    float width = 0.f;
    float height = 0.f;
};

class font_stack {
public:
    /// Process-wide shaping cache — face loading is expensive, share it.
    [[nodiscard]] static font_stack& shared();

    void load_default();
    void load(font_desc desc);
    [[nodiscard]] glyph_run shape(const char* utf8, font_desc desc) const;
    [[nodiscard]] float measure_width(const char* utf8, font_desc desc) const;

    /// True when a real font face backs shaping; false means metric estimation.
    [[nodiscard]] bool has_face() const;

private:
    gfx::glyph_atlas atlas_{};
};

class text_layout {
public:
    void set_text(const char* utf8);
    void set_font(font_desc desc);
    void set_max_width(float width);
    [[nodiscard]] lx::size2i bounding_size() const;
    void paint(lx::gfx::render_pass& pass, lx::point2i origin, lx::color color) const;

private:
    void reshape();

    const char* text_ = "";
    font_desc font_{};
    float max_width_ = 0.f;
    glyph_run shaped_{};
};

// ── Single-line CPU rasterization ─────────────────────────────────────────────
//
// Shaping (HarfBuzz) plus grayscale glyph rendering (FreeType) into a caller-owned
// premultiplied ARGB buffer. For text that changes rarely and is drawn as a texture — a
// server-side title bar, a panel label — where one upload per change beats a draw per
// glyph per frame on every composite backend. No heap allocation per call: the shaped
// line is fixed storage, the glyph cache is allocated once and cleared wholesale when full.
// One thread at a time per process (calls are serialized on the face cache's lock).

inline constexpr unsigned k_max_line_glyphs = 512;

/// A shaped line, visual order. Advances and offsets are 26.6 fixed point (1/64 px).
struct shaped_line {
    unsigned count = 0;
    std::uint32_t glyph[k_max_line_glyphs]{};
    std::int32_t x_adv[k_max_line_glyphs]{};
    std::int32_t x_off[k_max_line_glyphs]{};
    std::int32_t y_off[k_max_line_glyphs]{};
    /// Byte offset of the glyph's cluster in the UTF-8 input.
    std::uint32_t cluster[k_max_line_glyphs]{};
    int width_px = 0;
    int ascent_px = 0;
    int descent_px = 0;
    font_desc font{};
};

/// How much of a shaped line fits a width: glyphs [0, glyph_end), then "…" if ellipsized.
struct line_fit {
    unsigned glyph_end = 0;
    /// Width of what is drawn, ellipsis included. Never more than the width asked for.
    int width_px = 0;
    bool ellipsized = false;
};

/// Premultiplied ARGB (0xAARRGGBB) pixels, `stride_px` words per row.
struct raster_target {
    std::uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride_px = 0;
};

/// sRGB-encoded, straight alpha.
struct raster_color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

/// Shapes `utf8` with `font` into `out`. Lines longer than `k_max_line_glyphs` glyphs are
/// cut at the last whole cluster that fits. `not_implemented` without HarfBuzz/FreeType;
/// an `io` error when no face resolves for `font`.
[[nodiscard]] lx::result<void> shape_line(const char* utf8, font_desc font, shaped_line& out);
/// The longest prefix ending on a cluster boundary that fits `max_width_px` together with a
/// trailing "…" (U+2026); the whole line when it fits as is. Nothing (glyph_end 0, not
/// ellipsized) when not even the ellipsis fits.
[[nodiscard]] line_fit fit_line(const shaped_line& line, int max_width_px);
/// Composites `fit` of `line` source-over into `target` with the pen starting at
/// `baseline` (x, and the baseline's y). Pixels outside the target are clipped.
[[nodiscard]] lx::result<void> render_line(const shaped_line& line, line_fit fit,
                                           raster_color color, raster_target target,
                                           lx::point2i baseline);

} // namespace lx::text


#if defined(LUMEN_HAS_TEXT)
namespace lx::text::detail {

/// Owns FreeType + the resolved hb_font_t. Shaping without a face is never attempted:
/// hb_shape() dereferences its font argument, so a null face must fall back to metrics.
class face_cache {
public:
    static face_cache& instance() {
        static face_cache c{};
        return c;
    }

    /// Function-local static: runs at exit, so FreeType/HarfBuzz handles and the
    /// fontconfig global config are released rather than reported as leaks.
    ~face_cache() {
        std::lock_guard lock{mutex_};
        release_locked();
        if (library_) {
            FT_Done_FreeType(library_);
            library_ = nullptr;
        }
        if (init_attempted_ && fontconfig_ready_)
            FcFini();
    }

    face_cache(const face_cache&) = delete;
    face_cache& operator=(const face_cache&) = delete;

    hb_font_t* acquire(const font_desc& desc) {
        std::lock_guard lock{mutex_};
        return acquire_locked(desc);
    }

    /// Callers that use the face beyond one call (shaping, glyph rendering) hold this for
    /// the whole operation: FreeType faces are not thread-safe, and the single cached face
    /// may otherwise be replaced underneath them.
    [[nodiscard]] std::unique_lock<std::mutex> lock() { return std::unique_lock{mutex_}; }
    [[nodiscard]] FT_Face face_locked() const { return ft_face_; }
    /// Changes whenever a different face is loaded; glyph caches key on it.
    [[nodiscard]] unsigned generation_locked() const { return generation_; }

    hb_font_t* acquire_locked(const font_desc& desc) {
        if (!init_attempted_) {
            init_attempted_ = true;
            if (FT_Init_FreeType(&library_) != 0)
                library_ = nullptr;
            if (!FcInit())
                fontconfig_ready_ = false;
        }
        if (!library_ || !fontconfig_ready_)
            return nullptr;

        const int px = static_cast<int>(desc.size_pt * 64.f);
        const char* family = desc.family && desc.family[0] ? desc.family : "Sans";
        // By value, not by pointer: two equal family strings at different addresses are the
        // same face, and a caller's buffer may not outlive the cache entry.
        if (hb_font_ && cached_px_ == px && std::strcmp(cached_family_, family) == 0 &&
            cached_weight_ == desc.weight && cached_style_ == desc.style)
            return hb_font_;

        release_locked();

        FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(
            desc.family && desc.family[0] ? desc.family : "Sans"));
        if (!pattern)
            return nullptr;
        FcPatternAddInteger(pattern, FC_WEIGHT,
                            desc.weight == weight::bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
        FcPatternAddInteger(pattern, FC_SLANT,
                            desc.style == style::italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);

        FcResult match_result{};
        FcPattern* matched = FcFontMatch(nullptr, pattern, &match_result);
        FcPatternDestroy(pattern);
        if (!matched || match_result != FcResultMatch) {
            if (matched) FcPatternDestroy(matched);
            return nullptr;
        }

        FcChar8* file = nullptr;
        int face_index = 0;
        if (FcPatternGetString(matched, FC_FILE, 0, &file) != FcResultMatch || !file) {
            FcPatternDestroy(matched);
            return nullptr;
        }
        FcPatternGetInteger(matched, FC_INDEX, 0, &face_index);

        FT_Face face = nullptr;
        const FT_Error err =
            FT_New_Face(library_, reinterpret_cast<const char*>(file), face_index, &face);
        FcPatternDestroy(matched);
        if (err != 0 || !face)
            return nullptr;

        FT_Set_Char_Size(face, 0, px, 0, 96);
        hb_font_t* font = hb_ft_font_create_referenced(face);
        if (!font) {
            FT_Done_Face(face);
            return nullptr;
        }

        ft_face_ = face;
        hb_font_ = font;
        cached_px_ = px;
        std::strncpy(cached_family_, family, sizeof(cached_family_) - 1);
        cached_family_[sizeof(cached_family_) - 1] = '\0';
        ++generation_;
        cached_weight_ = desc.weight;
        cached_style_ = desc.style;
        return hb_font_;
    }

private:
    face_cache() = default;

    void release_locked() {
        if (hb_font_) {
            hb_font_destroy(hb_font_);
            hb_font_ = nullptr;
        }
        if (ft_face_) {
            FT_Done_Face(ft_face_);
            ft_face_ = nullptr;
        }
    }

    std::mutex mutex_{};
    FT_Library library_ = nullptr;
    FT_Face ft_face_ = nullptr;
    hb_font_t* hb_font_ = nullptr;
    int cached_px_ = 0;
    char cached_family_[128]{};
    unsigned generation_ = 0;
    weight cached_weight_ = weight::normal;
    style cached_style_ = style::normal;
    bool init_attempted_ = false;
    bool fontconfig_ready_ = true;
};

} // namespace lx::text::detail
#endif

namespace lx::text::detail {

/// Monospace-ish approximation used when no font face is available (headless CI).
[[nodiscard]] inline glyph_run estimate(const char* utf8, const font_desc& desc) {
    float w = 0.f;
    for (const char* p = utf8; *p; ++p) {
        // Count UTF-8 lead bytes only, so multibyte text is not over-measured.
        if ((static_cast<unsigned char>(*p) & 0xC0u) != 0x80u)
            w += desc.size_pt * 0.55f;
    }
    return {utf8, w, desc.size_pt * 1.2f};
}

} // namespace lx::text::detail


lx::text::font_stack& lx::text::font_stack::shared() {
    static font_stack instance{};
    return instance;
}

void lx::text::font_stack::load_default() { load({}); }

void lx::text::font_stack::load(font_desc desc) {
#if defined(LUMEN_HAS_TEXT)
    (void)detail::face_cache::instance().acquire(desc);
#else
    (void)desc;
#endif
}

bool lx::text::font_stack::has_face() const {
#if defined(LUMEN_HAS_TEXT)
    return detail::face_cache::instance().acquire(font_desc{}) != nullptr;
#else
    return false;
#endif
}

lx::text::glyph_run lx::text::font_stack::shape(const char* utf8, font_desc desc) const {
    if (!utf8 || utf8[0] == '\0')
        return {};
#if defined(LUMEN_HAS_TEXT)
    hb_font_t* font = detail::face_cache::instance().acquire(desc);
    if (!font)
        return detail::estimate(utf8, desc);

    hb_buffer_t* buf = hb_buffer_create();
    if (!buf)
        return detail::estimate(utf8, desc);
    hb_buffer_add_utf8(buf, utf8, -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(font, buf, nullptr, 0);

    unsigned count = 0;
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
    float width = 0.f;
    for (unsigned i = 0; i < count && pos; ++i)
        width += static_cast<float>(pos[i].x_advance) / 64.f;
    hb_buffer_destroy(buf);

    return {utf8, width, desc.size_pt * 1.2f};
#else
    return detail::estimate(utf8, desc);
#endif
}

float lx::text::font_stack::measure_width(const char* utf8, font_desc desc) const {
    return shape(utf8, desc).width;
}

void lx::text::text_layout::reshape() {
    shaped_ = font_stack::shared().shape(text_, font_);
}

void lx::text::text_layout::set_text(const char* t) {
    text_ = t ? t : "";
    reshape();
}

void lx::text::text_layout::set_font(font_desc d) {
    font_ = d;
    reshape();
}

void lx::text::text_layout::set_max_width(float w) { max_width_ = w; }

lx::size2i lx::text::text_layout::bounding_size() const {
    const float height = shaped_.height > 0.f ? shaped_.height : font_.size_pt * 1.2f;
    float width = shaped_.width;
    if (max_width_ > 0.f && width > max_width_)
        width = max_width_;
    return {static_cast<int>(width), static_cast<int>(height)};
}

void lx::text::text_layout::paint(lx::gfx::render_pass& pass, lx::point2i origin,
                                  lx::color color) const {
    (void)pass;
    (void)origin;
    (void)color;
}

// ── Single-line rasterization ─────────────────────────────────────────────────

#if defined(LUMEN_HAS_TEXT)
namespace lx::text::detail {

/// Grayscale glyph coverage, keyed by (face generation, glyph index). A fixed table and a
/// fixed arena, both allocated on first use and cleared wholesale when either fills: a
/// title bar or a panel touches a few hundred glyphs, so the steady state never clears.
class glyph_cache {
public:
    struct entry {
        unsigned generation = 0;
        unsigned glyph = 0;
        int left = 0;   ///< bitmap_left: pen → first column
        int top = 0;    ///< bitmap_top: baseline → first row (up is positive)
        unsigned width = 0;
        unsigned rows = 0;
        unsigned offset = 0; ///< into the arena; width*rows bytes
        bool used = false;
    };

    static glyph_cache& instance() {
        static glyph_cache c{};
        return c;
    }

    /// Caller holds the face cache lock. Null when FreeType cannot render the glyph.
    const entry* get(FT_Face face, unsigned generation, unsigned glyph) {
        if (!face)
            return nullptr;
        if (!arena_) {
            arena_ = new (std::nothrow) unsigned char[k_arena_bytes];
            if (!arena_)
                return nullptr;
        }
        unsigned h = (generation * 2654435761u) ^ (glyph * 40503u);
        for (unsigned probe = 0; probe < k_slots; ++probe) {
            entry& e = slots_[(h + probe) & (k_slots - 1)];
            if (!e.used)
                break;
            if (e.generation == generation && e.glyph == glyph)
                return &e;
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER) != 0)
            return nullptr;
        const FT_Bitmap& bm = face->glyph->bitmap;
        if (bm.pixel_mode != FT_PIXEL_MODE_GRAY && bm.pixel_mode != FT_PIXEL_MODE_MONO)
            return nullptr; // colour (emoji) bitmaps are not drawn by this path
        const unsigned bytes = bm.width * bm.rows;
        if (used_count_ + 1 > k_slots * 3 / 4 || arena_used_ + bytes > k_arena_bytes)
            clear();
        if (bytes > k_arena_bytes)
            return nullptr;
        entry* slot = nullptr;
        h = (generation * 2654435761u) ^ (glyph * 40503u);
        for (unsigned probe = 0; probe < k_slots; ++probe) {
            entry& e = slots_[(h + probe) & (k_slots - 1)];
            if (!e.used) {
                slot = &e;
                break;
            }
        }
        if (!slot)
            return nullptr;
        unsigned char* dst = arena_ + arena_used_;
        const int pitch = bm.pitch;
        for (unsigned r = 0; r < bm.rows; ++r) {
            const unsigned char* src =
                pitch >= 0 ? bm.buffer + static_cast<long>(r) * pitch
                           : bm.buffer + static_cast<long>(bm.rows - 1 - r) * -pitch;
            for (unsigned c = 0; c < bm.width; ++c) {
                dst[r * bm.width + c] =
                    bm.pixel_mode == FT_PIXEL_MODE_GRAY
                        ? src[c]
                        : ((src[c >> 3] >> (7 - (c & 7))) & 1u) ? 255u : 0u;
            }
        }
        slot->generation = generation;
        slot->glyph = glyph;
        slot->left = face->glyph->bitmap_left;
        slot->top = face->glyph->bitmap_top;
        slot->width = bm.width;
        slot->rows = bm.rows;
        slot->offset = arena_used_;
        slot->used = true;
        arena_used_ += bytes;
        ++used_count_;
        return slot;
    }

    [[nodiscard]] const unsigned char* coverage(const entry& e) const { return arena_ + e.offset; }

private:
    static constexpr unsigned k_slots = 1024;
    static constexpr unsigned k_arena_bytes = 1u << 20;

    void clear() {
        for (auto& e : slots_)
            e = {};
        arena_used_ = 0;
        used_count_ = 0;
    }

    entry slots_[k_slots]{};
    unsigned char* arena_ = nullptr; // process lifetime
    unsigned arena_used_ = 0;
    unsigned used_count_ = 0;
};

/// Reused for every shaping call (reset, never recreated). Guarded by the face cache lock.
inline hb_buffer_t* shared_buffer() {
    static hb_buffer_t* buf = hb_buffer_create();
    return buf;
}

/// "…" shaped once per face.
struct ellipsis_run {
    unsigned generation = ~0u;
    unsigned count = 0;
    std::uint32_t glyph[4]{};
    std::int32_t x_adv[4]{};
    std::int32_t x_off[4]{};
    std::int32_t y_off[4]{};
    int width_px = 0;
};

inline const ellipsis_run* ellipsis_locked(hb_font_t* font, unsigned generation) {
    static ellipsis_run run{};
    if (run.generation == generation)
        return &run;
    hb_buffer_t* buf = shared_buffer();
    if (!buf)
        return nullptr;
    hb_buffer_reset(buf);
    hb_buffer_add_utf8(buf, "\xE2\x80\xA6", -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(font, buf, nullptr, 0);
    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    run = {};
    run.generation = generation;
    long pen = 0;
    for (unsigned i = 0; i < n && i < 4 && info && pos; ++i) {
        run.glyph[i] = info[i].codepoint;
        run.x_adv[i] = pos[i].x_advance;
        run.x_off[i] = pos[i].x_offset;
        run.y_off[i] = pos[i].y_offset;
        pen += pos[i].x_advance;
        ++run.count;
    }
    run.width_px = static_cast<int>((pen + 63) / 64);
    return &run;
}

[[nodiscard]] inline int round_26_6(long v) {
    return static_cast<int>(v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64));
}

/// Source-over of `coverage` × `color` at (x0, y0), clipped to the target.
inline void blend_glyph(const glyph_cache::entry& g, const unsigned char* coverage,
                        raster_color color, raster_target& t, int x0, int y0) {
    for (unsigned r = 0; r < g.rows; ++r) {
        const int y = y0 + static_cast<int>(r);
        if (y < 0 || y >= t.height)
            continue;
        std::uint32_t* row = t.pixels + static_cast<long>(y) * t.stride_px;
        for (unsigned c = 0; c < g.width; ++c) {
            const int x = x0 + static_cast<int>(c);
            if (x < 0 || x >= t.width)
                continue;
            const unsigned cov = coverage[r * g.width + c];
            if (cov == 0)
                continue;
            const unsigned a = (static_cast<unsigned>(color.a) * cov + 127u) / 255u;
            const unsigned sr = (static_cast<unsigned>(color.r) * a + 127u) / 255u;
            const unsigned sg = (static_cast<unsigned>(color.g) * a + 127u) / 255u;
            const unsigned sb = (static_cast<unsigned>(color.b) * a + 127u) / 255u;
            const std::uint32_t d = row[x];
            const unsigned inv = 255u - a;
            const auto ch = [&](unsigned shift, unsigned s) {
                return s + (((d >> shift) & 0xFFu) * inv + 127u) / 255u;
            };
            row[x] = (ch(24, a) << 24) | (ch(16, sr) << 16) | (ch(8, sg) << 8) | ch(0, sb);
        }
    }
}

} // namespace lx::text::detail
#endif

lx::result<void> lx::text::shape_line(const char* utf8, font_desc font, shaped_line& out) {
    out.count = 0;
    out.width_px = 0;
    out.ascent_px = 0;
    out.descent_px = 0;
    out.font = font;
#if defined(LUMEN_HAS_TEXT)
    auto& cache = detail::face_cache::instance();
    auto lock = cache.lock();
    hb_font_t* hb = cache.acquire_locked(font);
    FT_Face face = cache.face_locked();
    if (!hb || !face)
        return lx::make_error(lx::error_domain::io, 0, "no font face resolved");
    const FT_Size_Metrics& m = face->size->metrics;
    out.ascent_px = static_cast<int>((m.ascender + 63) / 64);
    out.descent_px = static_cast<int>((-m.descender + 63) / 64);
    if (!utf8 || utf8[0] == '\0')
        return {};
    hb_buffer_t* buf = detail::shared_buffer();
    if (!buf)
        return lx::make_error(lx::error_domain::io, 0, "harfbuzz buffer allocation failed");
    hb_buffer_reset(buf);
    hb_buffer_add_utf8(buf, utf8, -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(hb, buf, nullptr, 0);
    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    if (!info || !pos)
        return {};
    unsigned count = n < k_max_line_glyphs ? n : k_max_line_glyphs;
    // Over capacity: drop the partial cluster at the cut too.
    if (count < n)
        while (count > 0 && info[count].cluster == info[count - 1].cluster)
            --count;
    long pen = 0;
    for (unsigned i = 0; i < count; ++i) {
        out.glyph[i] = info[i].codepoint;
        out.cluster[i] = info[i].cluster;
        out.x_adv[i] = pos[i].x_advance;
        out.x_off[i] = pos[i].x_offset;
        out.y_off[i] = pos[i].y_offset;
        pen += pos[i].x_advance;
    }
    out.count = count;
    out.width_px = static_cast<int>((pen + 63) / 64);
    return {};
#else
    (void)utf8;
    return lx::not_implemented("lx::text::shape_line");
#endif
}

lx::text::line_fit lx::text::fit_line(const shaped_line& line, int max_width_px) {
    if (line.count == 0 || max_width_px <= 0)
        return {};
    if (line.width_px <= max_width_px)
        return {line.count, line.width_px, false};
#if defined(LUMEN_HAS_TEXT)
    auto& cache = detail::face_cache::instance();
    auto lock = cache.lock();
    hb_font_t* hb = cache.acquire_locked(line.font);
    if (!hb)
        return {};
    const detail::ellipsis_run* ell = detail::ellipsis_locked(hb, cache.generation_locked());
    if (!ell || ell->width_px > max_width_px)
        return {};
    const int avail = max_width_px - ell->width_px;
    unsigned best = 0;
    int best_w = 0;
    long pen = 0;
    for (unsigned i = 0; i < line.count; ++i) {
        // A cut may fall only where a new cluster starts: never inside a ligature or between
        // a base and its combining marks.
        if (i > 0 && line.cluster[i] != line.cluster[i - 1]) {
            const int w = static_cast<int>((pen + 63) / 64);
            if (w > avail)
                break;
            best = i;
            best_w = w;
        }
        pen += line.x_adv[i];
    }
    return {best, best_w + ell->width_px, true};
#else
    return {};
#endif
}

lx::result<void> lx::text::render_line(const shaped_line& line, line_fit fit, raster_color color,
                                       raster_target target, lx::point2i baseline) {
    if (!target.pixels || target.width <= 0 || target.height <= 0 ||
        target.stride_px < target.width)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "render_line: bad target");
#if defined(LUMEN_HAS_TEXT)
    auto& cache = detail::face_cache::instance();
    auto lock = cache.lock();
    hb_font_t* hb = cache.acquire_locked(line.font);
    FT_Face face = cache.face_locked();
    if (!hb || !face)
        return lx::make_error(lx::error_domain::io, 0, "no font face resolved");
    const unsigned generation = cache.generation_locked();
    auto& glyphs = detail::glyph_cache::instance();
    long pen = 0;
    const auto draw = [&](std::uint32_t glyph, std::int32_t adv, std::int32_t xo,
                          std::int32_t yo) {
        if (const auto* g = glyphs.get(face, generation, glyph)) {
            const int x = baseline.x + detail::round_26_6(pen + xo) + g->left;
            const int y = baseline.y - detail::round_26_6(yo) - g->top;
            detail::blend_glyph(*g, glyphs.coverage(*g), color, target, x, y);
        }
        pen += adv;
    };
    const unsigned end = fit.glyph_end < line.count ? fit.glyph_end : line.count;
    for (unsigned i = 0; i < end; ++i)
        draw(line.glyph[i], line.x_adv[i], line.x_off[i], line.y_off[i]);
    if (fit.ellipsized) {
        // Placed where the kept prefix ends, rounded as `fit_line` measured it.
        pen = static_cast<long>((pen + 63) / 64) * 64;
        if (const detail::ellipsis_run* ell = detail::ellipsis_locked(hb, generation))
            for (unsigned i = 0; i < ell->count; ++i)
                draw(ell->glyph[i], ell->x_adv[i], ell->x_off[i], ell->y_off[i]);
    }
    return {};
#else
    (void)line;
    (void)fit;
    (void)color;
    (void)baseline;
    return lx::not_implemented("lx::text::render_line");
#endif
}
