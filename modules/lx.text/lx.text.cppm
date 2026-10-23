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

// ── Wrapped paragraphs (a notification's body, a tooltip) ─────────────────────
//
// Greedy line breaking over styled spans: break opportunities after spaces, after a hyphen
// or a slash, around CJK characters, and at '\n' (a hard break); a word wider than the line
// is broken between clusters. Each piece (one word, or its part in one span) is shaped on
// its own with the span's font; the widths add up per line. Lines beyond `max_lines` are
// dropped and the last kept line ends in "…" (cut at a cluster boundary). Left-to-right line
// order (a right-to-left run is shaped in its own visual order within its piece). Fixed
// storage, no heap; one thread at a time per process, as the single-line calls.

/// A styled stretch [begin, end) of the paragraph's UTF-8 bytes. Bytes no span covers use
/// the base font.
struct text_span {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    weight weight = weight::normal;
    style style = style::normal;
    bool underline = false;
    /// The caller's tag (a colour, a link number), handed back when drawing.
    std::uint16_t tag = 0;
};

inline constexpr unsigned k_max_wrap_lines = 24;
inline constexpr unsigned k_max_wrap_pieces = 320;
/// The longest piece shaped at once (bytes); longer words are broken.
inline constexpr unsigned k_max_wrap_piece_bytes = 480;

/// A part of one span on one line.
struct wrap_piece {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    /// Index into the spans, or 0xFFFF: the base font.
    std::uint16_t span = 0xFFFF;
    /// "…" is drawn after this piece (the last piece of a cut paragraph).
    bool ellipsis = false;
    /// From the line's start; the width includes the ellipsis.
    int x_px = 0;
    int width_px = 0;
};

struct wrap_line {
    unsigned first = 0;
    unsigned count = 0;
    /// Without trailing spaces.
    int width_px = 0;
};

struct wrapped_text {
    unsigned line_count = 0;
    wrap_line lines[k_max_wrap_lines]{};
    unsigned piece_count = 0;
    wrap_piece pieces[k_max_wrap_pieces]{};
    /// The base font's metrics; a line advances `line_height_px`.
    int ascent_px = 0;
    int descent_px = 0;
    int line_height_px = 0;
    /// The widest line.
    int width_px = 0;
    /// Not everything fitted (`max_lines`, or the piece storage): the last line ends in "…".
    bool truncated = false;
};

/// Lays out `utf8` with `spans` over `base` in lines at most `max_width_px` wide, at most
/// `max_lines` (≤ k_max_wrap_lines) of them. `not_implemented` without HarfBuzz/FreeType; an
/// `io` error when no face resolves.
[[nodiscard]] lx::result<void> wrap_text(std::string_view utf8, std::span<const text_span> spans,
                                         font_desc base, int max_width_px, unsigned max_lines,
                                         wrapped_text& out);
/// Each span's colour (by its tag; `span` < 0: the base font's text).
using span_color_fn = raster_color (*)(int span, std::uint16_t tag, void* user);
/// Draws `w` into `target`: line i's baseline at origin.y + ascent + i × line height; its
/// pieces at origin.x + their x. Underlined spans get a line under their ink.
[[nodiscard]] lx::result<void> render_wrapped(std::string_view utf8,
                                              std::span<const text_span> spans,
                                              const wrapped_text& w, font_desc base,
                                              span_color_fn color, void* user,
                                              raster_target target, lx::point2i origin);
/// The piece of `w` under (x, y) (relative to the origin `render_wrapped` was given), or -1.
[[nodiscard]] int wrapped_piece_at(const wrapped_text& w, int x, int y);

} // namespace lx::text


#if defined(LUMEN_HAS_TEXT)
namespace lx::text::detail {

/// Owns FreeType and a few resolved faces (hb_font_t over FT_Face), least recently used
/// first out. Shaping without a face is never attempted: hb_shape() dereferences its font
/// argument, so a null face must fall back to metrics.
///
/// Several faces stay loaded because text mixes them: a bold title over a regular body, an
/// italic span, the panel's letter tiles. With one face every switch was a fontconfig match
/// plus FT_New_Face (milliseconds), and the glyph cache, keyed by face, lost its entries.
/// Each entry has its own generation (never reused), so cached glyphs of one face stay valid
/// while another is in use.
class face_cache {
public:
    static constexpr unsigned k_faces = 8;

    static face_cache& instance() {
        static face_cache c{};
        return c;
    }

    /// Function-local static: runs at exit, so FreeType/HarfBuzz handles and the
    /// fontconfig global config are released rather than reported as leaks.
    ~face_cache() {
        std::lock_guard lock{mutex_};
        for (auto& e : faces_)
            release_locked(e);
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
    /// the whole operation: FreeType faces are not thread-safe, and the face in use may
    /// otherwise be evicted underneath them.
    [[nodiscard]] std::unique_lock<std::mutex> lock() { return std::unique_lock{mutex_}; }
    /// The face the last `acquire_locked` returned.
    [[nodiscard]] FT_Face face_locked() const {
        return current_ < k_faces ? faces_[current_].ft : nullptr;
    }
    /// Identifies the face the last `acquire_locked` returned (unique per loaded face);
    /// glyph caches key on it.
    [[nodiscard]] unsigned generation_locked() const {
        return current_ < k_faces ? faces_[current_].generation : 0u;
    }

    hb_font_t* acquire_locked(const font_desc& desc) {
        if (!init_attempted_) {
            init_attempted_ = true;
            if (FT_Init_FreeType(&library_) != 0)
                library_ = nullptr;
            if (!FcInit())
                fontconfig_ready_ = false;
        }
        current_ = k_faces;
        if (!library_ || !fontconfig_ready_)
            return nullptr;

        const int px = static_cast<int>(desc.size_pt * 64.f);
        const char* family = desc.family && desc.family[0] ? desc.family : "Sans";
        // By value, not by pointer: two equal family strings at different addresses are the
        // same face, and a caller's buffer may not outlive the cache entry.
        const char* file = nullptr;
        int face_index = 0;
        unsigned victim = 0;
        for (unsigned i = 0; i < k_faces; ++i) {
            entry& e = faces_[i];
            if (!e.hb) {
                victim = i;
                continue;
            }
            const bool same_face = std::strcmp(e.family, family) == 0 &&
                                   e.weight == desc.weight && e.style == desc.style;
            if (same_face && e.px == px) {
                e.used = ++clock_;
                current_ = i;
                return e.hb;
            }
            // The same face at another size (a fractional scale): its file is known.
            if (same_face && e.file[0]) {
                file = e.file;
                face_index = e.face_index;
            }
            if (faces_[victim].hb && e.used < faces_[victim].used)
                victim = i;
        }

        char resolved[sizeof(entry::file)];
        if (file) {
            std::snprintf(resolved, sizeof(resolved), "%s", file);
        } else {
            FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(family));
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
            FcChar8* match_file = nullptr;
            if (FcPatternGetString(matched, FC_FILE, 0, &match_file) != FcResultMatch ||
                !match_file) {
                FcPatternDestroy(matched);
                return nullptr;
            }
            FcPatternGetInteger(matched, FC_INDEX, 0, &face_index);
            std::snprintf(resolved, sizeof(resolved), "%s", reinterpret_cast<const char*>(match_file));
            FcPatternDestroy(matched);
        }

        FT_Face face = nullptr;
        const FT_Error err = FT_New_Face(library_, resolved, face_index, &face);
        if (err != 0 || !face)
            return nullptr;

        FT_Set_Char_Size(face, 0, px, 0, 96);
        hb_font_t* font = hb_ft_font_create_referenced(face);
        if (!font) {
            FT_Done_Face(face);
            return nullptr;
        }

        entry& e = faces_[victim];
        release_locked(e);
        e.ft = face;
        e.hb = font;
        e.px = px;
        std::snprintf(e.family, sizeof(e.family), "%s", family);
        std::snprintf(e.file, sizeof(e.file), "%s", resolved);
        e.face_index = face_index;
        e.weight = desc.weight;
        e.style = desc.style;
        e.generation = ++generation_;
        e.used = ++clock_;
        current_ = victim;
        return e.hb;
    }

private:
    struct entry {
        FT_Face ft = nullptr;
        hb_font_t* hb = nullptr;
        int px = 0;
        char family[128]{};
        /// The file fontconfig resolved (reused for the same face at another size).
        char file[512]{};
        int face_index = 0;
        weight weight = weight::normal;
        style style = style::normal;
        unsigned generation = 0;
        unsigned long long used = 0;
    };

    face_cache() = default;

    static void release_locked(entry& e) {
        if (e.hb)
            hb_font_destroy(e.hb);
        if (e.ft)
            FT_Done_Face(e.ft);
        e = entry{};
    }

    std::mutex mutex_{};
    FT_Library library_ = nullptr;
    entry faces_[k_faces]{};
    unsigned current_ = k_faces;
    unsigned generation_ = 0;
    unsigned long long clock_ = 0;
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
        return lx::invalid_argument("render_line: bad target");
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

// ── Wrapped paragraphs ────────────────────────────────────────────────────────

namespace {

constexpr std::uint16_t k_base_span = 0xFFFF;

/// The code point at `i` (advanced past it); U+FFFD for a malformed sequence.
std::uint32_t next_code_point(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i++]);
    if (b < 0x80)
        return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    std::uint32_t cp = extra == 3 ? b & 0x07u : extra == 2 ? b & 0x0Fu : b & 0x1Fu;
    if (extra == 0)
        return 0xFFFD;
    while (extra-- > 0) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0u) != 0x80u)
            return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3Fu);
    }
    return cp;
}

/// Ideographs, kana, Hangul and full-width forms: a line may break before and after each.
bool breaks_around(std::uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) ||
           (c >= 0x20000 && c <= 0x2FFFF);
}

bool is_space(std::uint32_t c) { return c == ' ' || c == '\t'; }

[[maybe_unused]] lx::text::font_desc span_font(lx::text::font_desc base,
                                               std::span<const lx::text::text_span> spans,
                                               std::uint16_t span) {
    if (span < spans.size()) {
        base.weight = spans[span].weight;
        base.style = spans[span].style;
    }
    return base;
}

/// The span covering byte `at` and where its style ends (`limit` at most).
[[maybe_unused]] std::uint16_t span_at(std::span<const lx::text::text_span> spans, std::uint32_t at,
                                       std::uint32_t limit, std::uint32_t& run_end) {
    std::uint32_t next = limit;
    for (std::size_t i = 0; i < spans.size() && i < k_base_span; ++i) {
        const auto& s = spans[i];
        if (at >= s.begin && at < s.end) {
            run_end = s.end < limit ? s.end : limit;
            return static_cast<std::uint16_t>(i);
        }
        if (s.begin > at && s.begin < next)
            next = s.begin;
    }
    run_end = next;
    return k_base_span;
}

/// A NUL-terminated copy of [b, e) (cut to the buffer at a code point boundary).
[[maybe_unused]] const char* piece_text(std::string_view s, std::uint32_t b, std::uint32_t e,
                                        char (&buf)[lx::text::k_max_wrap_piece_bytes + 1]) {
    std::size_t n = e > b ? e - b : 0;
    if (n > lx::text::k_max_wrap_piece_bytes) {
        n = lx::text::k_max_wrap_piece_bytes;
        while (n > 0 && (static_cast<unsigned char>(s[b + n]) & 0xC0u) == 0x80u)
            --n;
    }
    std::memcpy(buf, s.data() + b, n);
    buf[n] = '\0';
    return buf;
}

} // namespace

#if defined(LUMEN_HAS_TEXT)
namespace {

/// The layout in progress: lines and pieces into `out`.
struct wrap_state {
    std::string_view text;
    std::span<const lx::text::text_span> spans;
    lx::text::font_desc base;
    int max_w = 0;
    unsigned max_lines = 1;
    lx::text::wrapped_text* out = nullptr;
    /// The current line's pen (trailing spaces included) and its inked width.
    int pen = 0;
    int ink = 0;
    bool full = false;
    lx::text::shaped_line scratch{};

    lx::text::wrap_line& line() { return out->lines[out->line_count - 1]; }

    /// Width of [b, e) in `span`'s font; -1 when it cannot be shaped.
    int measure(std::uint32_t b, std::uint32_t e, std::uint16_t span) {
        char buf[lx::text::k_max_wrap_piece_bytes + 1];
        if (!lx::text::shape_line(piece_text(text, b, e, buf), span_font(base, spans, span), scratch))
            return -1;
        return scratch.width_px;
    }

    bool new_line() {
        if (out->line_count >= max_lines || out->line_count >= lx::text::k_max_wrap_lines) {
            full = true;
            return false;
        }
        lx::text::wrap_line& l = out->lines[out->line_count++];
        l = {};
        l.first = out->piece_count;
        pen = ink = 0;
        return true;
    }

    bool add_piece(std::uint32_t b, std::uint32_t e, std::uint16_t span, int w) {
        if (out->piece_count >= lx::text::k_max_wrap_pieces) {
            full = true;
            return false;
        }
        lx::text::wrap_piece& p = out->pieces[out->piece_count++];
        p = {};
        p.begin = b;
        p.end = e;
        p.span = span;
        p.x_px = pen;
        p.width_px = w;
        ++line().count;
        pen += w;
        ink = pen;
        line().width_px = ink;
        return true;
    }

    /// Places the word [b, e) and its trailing spaces up to `next`.
    void place_word(std::uint32_t b, std::uint32_t e, std::uint32_t next) {
        // Segments by span, each measured once.
        struct seg {
            std::uint32_t b, e;
            std::uint16_t span;
            int w;
        };
        seg segs[24];
        unsigned n = 0;
        int word_w = 0;
        for (std::uint32_t at = b; at < e && n < 24;) {
            std::uint32_t run_end = e;
            const std::uint16_t span = span_at(spans, at, e, run_end);
            if (run_end - at > lx::text::k_max_wrap_piece_bytes) {
                run_end = at + lx::text::k_max_wrap_piece_bytes;
                while (run_end > at + 1 &&
                       (static_cast<unsigned char>(text[run_end]) & 0xC0u) == 0x80u)
                    --run_end;
            }
            const int w = measure(at, run_end, span);
            if (w < 0)
                return;
            segs[n++] = {at, run_end, span, w};
            word_w += w;
            at = run_end;
        }
        int space_w = 0;
        if (next > e) {
            std::uint32_t run_end = next;
            const int w = measure(e, next, span_at(spans, e, next, run_end));
            space_w = w > 0 ? w : 0;
        }
        if (line().count > 0 && pen + word_w > max_w && !new_line())
            return;
        for (unsigned i = 0; i < n && !full; ++i) {
            const seg& s = segs[i];
            if (pen + s.w <= max_w || (line().count == 0 && pen == 0 && s.w <= max_w)) {
                add_piece(s.b, s.e, s.span, s.w);
                continue;
            }
            // Wider than what is left: broken between clusters (an over-long word).
            split_segment(s.b, s.e, s.span);
        }
        pen += space_w;
    }

    /// A segment that does not fit: as much as fits on each line, at cluster boundaries.
    void split_segment(std::uint32_t b, std::uint32_t e, std::uint16_t span) {
        while (b < e && !full) {
            char buf[lx::text::k_max_wrap_piece_bytes + 1];
            const char* t = piece_text(text, b, e, buf);
            if (!lx::text::shape_line(t, span_font(base, spans, span), scratch))
                return;
            const int room = max_w - pen;
            if (scratch.width_px <= room) {
                add_piece(b, b + static_cast<std::uint32_t>(std::strlen(t)), span, scratch.width_px);
                b += static_cast<std::uint32_t>(std::strlen(t));
                continue;
            }
            // The longest cluster prefix that fits.
            long acc = 0;
            unsigned cut = 0;
            int cut_w = 0;
            for (unsigned g = 0; g < scratch.count; ++g) {
                if (g > 0 && scratch.cluster[g] != scratch.cluster[g - 1]) {
                    const int w = static_cast<int>((acc + 63) / 64);
                    if (w > room)
                        break;
                    cut = scratch.cluster[g];
                    cut_w = w;
                }
                acc += scratch.x_adv[g];
            }
            if (cut == 0) {
                if (line().count > 0) {
                    if (!new_line())
                        return;
                    continue;
                }
                // Not even one cluster fits an empty line: it goes anyway (clipped).
                std::size_t i = 0;
                (void)next_code_point(std::string_view{t}, i);
                cut = static_cast<unsigned>(i);
                cut_w = room;
            }
            add_piece(b, b + cut, span, cut_w);
            b += cut;
            if (b < e && !new_line())
                return;
        }
    }

    /// The last line ends in "…": its last pieces are cut until it fits.
    void ellipsize() {
        out->truncated = true;
        if (out->line_count == 0 && !new_line_forced())
            return;
        lx::text::wrap_line& l = line();
        while (true) {
            if (l.count == 0) {
                // Nothing left on the line: the ellipsis alone.
                if (out->piece_count >= lx::text::k_max_wrap_pieces)
                    return;
                const std::uint16_t span = k_base_span;
                if (!lx::text::shape_line("\xE2\x80\xA6", span_font(base, spans, span), scratch))
                    return;
                lx::text::wrap_piece& p = out->pieces[out->piece_count++];
                p = {};
                p.span = span;
                p.ellipsis = true;
                p.width_px = scratch.width_px;
                l.count = 1;
                l.width_px = p.width_px;
                return;
            }
            lx::text::wrap_piece& p = out->pieces[l.first + l.count - 1];
            const lx::text::font_desc f = span_font(base, spans, p.span);
            if (!lx::text::shape_line("\xE2\x80\xA6", f, scratch))
                return;
            const int ell = scratch.width_px;
            if (p.x_px + p.width_px + ell <= max_w) {
                p.width_px += ell;
                p.ellipsis = true;
                l.width_px = p.x_px + p.width_px;
                return;
            }
            char buf[lx::text::k_max_wrap_piece_bytes + 1];
            if (!lx::text::shape_line(piece_text(text, p.begin, p.end, buf), f, scratch))
                return;
            const lx::text::line_fit fit = lx::text::fit_line(scratch, max_w - p.x_px);
            if (fit.ellipsized && fit.glyph_end > 0) {
                p.end = p.begin + scratch.cluster[fit.glyph_end];
                p.width_px = fit.width_px;
                p.ellipsis = true;
                l.width_px = p.x_px + p.width_px;
                return;
            }
            // Nothing of this piece fits with the ellipsis: drop it.
            --l.count;
            --out->piece_count;
            l.width_px = l.count ? out->pieces[l.first + l.count - 1].x_px +
                                       out->pieces[l.first + l.count - 1].width_px
                                 : 0;
        }
    }

    bool new_line_forced() {
        full = false;
        return new_line();
    }
};

} // namespace
#endif

lx::result<void> lx::text::wrap_text(std::string_view text, std::span<const text_span> spans,
                                     font_desc base, int max_width_px, unsigned max_lines,
                                     wrapped_text& out) {
    out.line_count = 0;
    out.piece_count = 0;
    out.truncated = false;
    out.width_px = 0;
#if defined(LUMEN_HAS_TEXT)
    // Per thread, reused: the shaping scratch is 10 KiB, never allocated per call.
    static thread_local wrap_state s{};
    if (!lx::text::shape_line("", base, s.scratch))
        return lx::make_error(lx::error_domain::io, 0, "no font face resolved");
    out.ascent_px = s.scratch.ascent_px;
    out.descent_px = s.scratch.descent_px;
    const int h = out.ascent_px + out.descent_px;
    out.line_height_px = h + (h / 6 > 1 ? h / 6 : 1);
    s.text = text;
    s.spans = spans;
    s.base = base;
    s.max_w = max_width_px > 1 ? max_width_px : 1;
    s.max_lines = max_lines == 0 ? 1 : (max_lines > k_max_wrap_lines ? k_max_wrap_lines : max_lines);
    s.out = &out;
    s.pen = s.ink = 0;
    s.full = false;
    if (text.empty())
        return {};
    (void)s.new_line();
    // Units: a word and the spaces after it; a hard break at '\n'; soft breaks after a
    // hyphen or a slash and around CJK characters.
    std::size_t i = 0;
    std::uint32_t word_begin = 0;
    while (i < text.size() && !s.full) {
        const std::size_t at = i;
        const std::uint32_t c = next_code_point(text, i);
        if (c == '\n') {
            if (at > word_begin)
                s.place_word(word_begin, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(at));
            word_begin = static_cast<std::uint32_t>(i);
            if (i < text.size())
                (void)s.new_line();
            continue;
        }
        if (is_space(c)) {
            // The word ends here; its spaces run to the next non-space.
            std::size_t j = i;
            while (j < text.size()) {
                std::size_t k = j;
                const std::uint32_t d = next_code_point(text, k);
                if (!is_space(d))
                    break;
                j = k;
            }
            s.place_word(word_begin, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(j));
            word_begin = static_cast<std::uint32_t>(j);
            i = j;
            continue;
        }
        const bool around = breaks_around(c);
        if (around && at > word_begin) {
            s.place_word(word_begin, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(at));
            word_begin = static_cast<std::uint32_t>(at);
        }
        if (around || ((c == '-' || c == '/') && i < text.size() &&
                       !is_space(static_cast<unsigned char>(text[i])) && text[i] != '\n')) {
            s.place_word(word_begin, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i));
            word_begin = static_cast<std::uint32_t>(i);
        }
    }
    if (!s.full && word_begin < text.size())
        s.place_word(word_begin, static_cast<std::uint32_t>(text.size()),
                     static_cast<std::uint32_t>(text.size()));
    // A line left empty at the end (text ending in '\n') is not a line.
    while (out.line_count > 1 && out.lines[out.line_count - 1].count == 0 && !s.full)
        --out.line_count;
    if (s.full)
        s.ellipsize();
    for (unsigned l = 0; l < out.line_count; ++l)
        if (out.lines[l].width_px > out.width_px)
            out.width_px = out.lines[l].width_px;
    return {};
#else
    (void)text;
    (void)spans;
    (void)base;
    (void)max_width_px;
    (void)max_lines;
    return lx::not_implemented("lx::text::wrap_text");
#endif
}

lx::result<void> lx::text::render_wrapped(std::string_view text, std::span<const text_span> spans,
                                          const wrapped_text& w, font_desc base,
                                          span_color_fn color, void* user, raster_target target,
                                          lx::point2i origin) {
    if (!target.pixels || target.width <= 0 || target.height <= 0 ||
        target.stride_px < target.width)
        return lx::invalid_argument("render_wrapped: bad target");
#if defined(LUMEN_HAS_TEXT)
    static thread_local shaped_line line_storage{};
    shaped_line* line = &line_storage;
    char buf[k_max_wrap_piece_bytes + 1];
    for (unsigned l = 0; l < w.line_count; ++l) {
        const int baseline = origin.y + w.ascent_px + static_cast<int>(l) * w.line_height_px;
        const wrap_line& ln = w.lines[l];
        for (unsigned k = 0; k < ln.count && ln.first + k < w.piece_count; ++k) {
            const wrap_piece& p = w.pieces[ln.first + k];
            const font_desc f = span_font(base, spans, p.span);
            if (!shape_line(piece_text(text, p.begin, p.end, buf), f, *line))
                continue;
            const std::uint16_t tag = p.span < spans.size() ? spans[p.span].tag : 0;
            const raster_color c =
                color ? color(p.span < spans.size() ? static_cast<int>(p.span) : -1, tag, user)
                      : raster_color{0xE6, 0xE6, 0xE6, 0xFF};
            const line_fit fit{line->count, line->width_px, p.ellipsis};
            const int x = origin.x + p.x_px;
            (void)render_line(*line, fit, c, target, {x, baseline});
            if (p.span < spans.size() && spans[p.span].underline) {
                // Under the ink: a third of the descent below the baseline, ≥ 1 px thick.
                const int thick = line->ascent_px >= 16 ? 2 : 1;
                const int uy = baseline + (line->descent_px + 2) / 3;
                const unsigned a = c.a;
                for (int y = uy; y < uy + thick; ++y) {
                    if (y < 0 || y >= target.height)
                        continue;
                    std::uint32_t* row = target.pixels + static_cast<long>(y) * target.stride_px;
                    for (int xx = x; xx < x + p.width_px; ++xx) {
                        if (xx < 0 || xx >= target.width)
                            continue;
                        const std::uint32_t d = row[xx];
                        const unsigned inv = 255u - a;
                        const auto ch = [&](unsigned shift, unsigned s) {
                            return s + (((d >> shift) & 0xFFu) * inv + 127u) / 255u;
                        };
                        row[xx] = (ch(24, a) << 24) | (ch(16, (c.r * a + 127u) / 255u) << 16) |
                                  (ch(8, (c.g * a + 127u) / 255u) << 8) |
                                  ch(0, (c.b * a + 127u) / 255u);
                    }
                }
            }
        }
    }
    return {};
#else
    (void)text;
    (void)spans;
    (void)w;
    (void)base;
    (void)color;
    (void)user;
    (void)origin;
    return lx::not_implemented("lx::text::render_wrapped");
#endif
}

int lx::text::wrapped_piece_at(const wrapped_text& w, int x, int y) {
    if (y < 0 || w.line_height_px <= 0)
        return -1;
    const unsigned l = static_cast<unsigned>(y / w.line_height_px);
    if (l >= w.line_count)
        return -1;
    const wrap_line& ln = w.lines[l];
    for (unsigned k = 0; k < ln.count && ln.first + k < w.piece_count; ++k) {
        const wrap_piece& p = w.pieces[ln.first + k];
        if (x >= p.x_px && x < p.x_px + p.width_px)
            return static_cast<int>(ln.first + k);
    }
    return -1;
}
