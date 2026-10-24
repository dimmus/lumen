module;

#if defined(LUMEN_HAS_TEXT)
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <hb-ft.h>
#include <hb.h>
#endif
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

import lx.foundation;

export module lx.text:faces;

import std;

export namespace lx::text {

// ── Faces (ADR-010 §4.6 `:faces`) ────────────────────────────────────────────────
//
// Family resolution through fontconfig and loaded faces: an LRU of `k_face_cache_size`
// (family, weight, style) faces, each owning its FT_Face and the hb_font_t over it. A face is
// loaded once for every size: the size is set on use (`use_locked`, FT_Set_Char_Size only
// when it differs from the size the face was last used at), never baked into the cache key.
//
// Face ids are unique for the cache's lifetime: a slot reused for another face gets a new id,
// so anything keyed by a face id (the shape cache, the glyph cache) can never hit an entry of
// the face that used the slot before; the stale entries age out of their own LRUs.
//
// Bundled fonts (assets/fonts, ADR §5.4) are registered with FcConfigAppFontAddDir before
// the first resolution, from the first of: `LUMEN_FONT_DIR` (replaces the others; a
// directory that does not exist registers nothing, and resolution falls back to the system
// fonts), the build tree's assets/fonts, `<exe>/../${CMAKE_INSTALL_DATADIR}/lumen/fonts` (a
// relocated install), the configured install prefix's copy. Each cache owns its fontconfig
// configuration (the system one plus that directory), so the process-wide default config is
// never modified.
//
// Threading contract. One std::mutex per cache guards everything in it, and a caller that
// uses a face beyond one call (shaping, glyph loading) holds `lock()` for the whole
// operation: FreeType faces are not thread-safe, the size set by `use_locked` is state of the
// face, and a face may be evicted by the next resolution. Under the lock, the handles
// `use_locked` returns stay valid until the next `resolve_locked` on the same cache. The lock
// is uncontended in practice (UI thread; the worker affinity once large layouts are shaped
// there, ADR §4.6) and costs one atomic exchange; a hit in a shape cache never takes it. The
// process-wide `shared()` cache also guards `shape_cache::shared()` (see `:shape`).

enum class weight { normal, bold };
enum class style { normal, italic };

/// The UI family (ADR §5.4): IBM Plex Sans, bundled; fontconfig's `sans-serif` match when
/// it cannot be found.
inline constexpr const char* k_ui_family = "IBM Plex Sans";
/// The mono family: Lilex, bundled; then JetBrains Mono (either build), then `monospace`.
inline constexpr const char* k_mono_family = "Lilex";

struct font_desc {
    /// A fontconfig family name (or pattern). Null or empty is `k_ui_family`.
    const char* family = k_ui_family;
    /// Points at 96 dpi (pixels = size_pt × 4/3), the convention every caller sizes with.
    float size_pt = 11.f;
    weight weight = weight::normal;
    style style = style::normal;
};

/// The size faces and shape-cache keys take: the em size in pixels, 26.6 fixed point.
/// `size_pt` × scale goes in as points at 96 dpi; the rounding is FreeType's own
/// (FT_REQUEST_HEIGHT), so a face sized with this value at 72 dpi has exactly the metrics it
/// had sized with `size_pt` × 64 at 96 dpi.
[[nodiscard]] constexpr std::int32_t size_26_6_from_pt(float size_pt) {
    const auto pt_26_6 = static_cast<std::int64_t>(size_pt * 64.f);
    return pt_26_6 > 0 ? static_cast<std::int32_t>((pt_26_6 * 96 + 36) / 72) : 0;
}

inline constexpr unsigned k_face_cache_size = 32;
inline constexpr unsigned k_face_family_bytes = 128;
inline constexpr unsigned k_face_path_bytes = 512;

/// A loaded face. 0 is no face. Unique for the cache's lifetime (never reused).
struct face_id {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(face_id, face_id) = default;
};

/// A face's metrics at one size. Scaled values are 26.6 pixels; FreeType rounds ascent up
/// and descent down to whole pixels for scalable faces (FT_Size_Metrics).
struct face_metrics {
    std::uint16_t units_per_em = 0;
    std::int32_t size_26_6 = 0;
    /// Above the baseline, positive.
    std::int32_t ascent = 0;
    /// Below the baseline, positive.
    std::int32_t descent = 0;
    /// The face's line height minus ascent and descent (never negative).
    std::int32_t line_gap = 0;
    /// OS/2 sxHeight, or the height of "x" when the table has none; 0 when neither exists.
    std::int32_t x_height = 0;
    /// The face's underline: top edge's offset below the baseline (positive is down) and
    /// thickness (at least 1 px). From the `post` table; 1/14 and 1/20 em when it has none.
    std::int32_t underline_offset = 0;
    std::int32_t underline_thickness = 0;
};

/// What a face id resolved to.
struct face_info {
    char requested_family[k_face_family_bytes]{};
    /// The family fontconfig matched (FC_FAMILY of the match).
    char family[k_face_family_bytes]{};
    char file[k_face_path_bytes]{};
    int index = 0;
    weight weight = weight::normal;
    style style = style::normal;
};

struct face_cache_stats {
    /// `resolve` calls answered by a loaded face.
    std::uint64_t hits = 0;
    /// Faces loaded (a fontconfig match plus FT_New_Face each).
    std::uint64_t loads = 0;
    /// Loaded faces dropped to make room.
    std::uint64_t evictions = 0;
    /// Resolutions that found no face at all.
    std::uint64_t failures = 0;
    /// FT_Set_Char_Size calls (a use at a size other than the face's current one).
    std::uint64_t size_changes = 0;
    /// `fallback_locked` searches (each a fontconfig match over the charset).
    std::uint64_t fallback_searches = 0;
    /// Searches that found no scalable face with the character.
    std::uint64_t fallback_failures = 0;
    unsigned live = 0;
};

class face_cache {
public:
    /// Raw library handles of a face in use, as `void*` so no FreeType or HarfBuzz type
    /// crosses the module interface: `FT_Face` and `hb_font_t*`. Null when the id is stale.
    struct handles {
        void* ft_face = nullptr;
        void* hb_font = nullptr;
    };

    /// The process-wide cache.
    [[nodiscard]] static face_cache& shared();

    /// `font_dir`: the directory of fonts to register; null takes the search described
    /// above (`LUMEN_FONT_DIR`, then the build tree, then the install).
    explicit face_cache(const char* font_dir = nullptr);
    ~face_cache();

    face_cache(const face_cache&) = delete;
    face_cache& operator=(const face_cache&) = delete;

    /// The face for (family, weight, style), loaded if needed. Invalid when no face resolves
    /// (or built without HarfBuzz/FreeType). Takes the lock.
    [[nodiscard]] face_id resolve(const font_desc& desc);
    [[nodiscard]] face_id resolve(const char* family, weight w, style s);

    /// Everything below with `_locked` expects `lock()` held by the caller.
    [[nodiscard]] std::unique_lock<std::mutex> lock() { return std::unique_lock{mutex_}; }
    [[nodiscard]] face_id resolve_locked(const char* family, weight w, style s);
    /// Sets the face to `size_26_6` (only when it is not already at it) and hands out its
    /// handles; valid until the next `resolve_locked`.
    [[nodiscard]] handles use_locked(face_id id, std::int32_t size_26_6);
    [[nodiscard]] lx::result<face_metrics> metrics_locked(face_id id, std::int32_t size_26_6);
    /// The face maps `codepoint` to a glyph (its cmap; FT_Get_Char_Index). False for a stale id.
    [[nodiscard]] bool has_char_locked(face_id id, char32_t codepoint);
    /// `alive` with the lock held.
    [[nodiscard]] bool alive_locked(face_id id) { return find_locked(id) != nullptr; }
    /// A face for `codepoint` when `primary` lacks it (ADR §4.6 `:itemize`): fontconfig's best
    /// scalable match for the primary's requested family, weight and style whose charset has
    /// the character, loaded into the LRU like any face (keyed by its file, so it never answers
    /// a family resolution). Invalid when no installed scalable face has it, or `primary` is
    /// stale. One fontconfig match per call — callers cache the answer (`itemizer` does, per
    /// primary face, script and block).
    [[nodiscard]] face_id fallback_locked(face_id primary, char32_t codepoint);

    [[nodiscard]] lx::result<face_metrics> metrics(face_id id, std::int32_t size_26_6);
    /// False when `id` is not loaded (evicted, or never was).
    [[nodiscard]] bool alive(face_id id);
    [[nodiscard]] bool describe(face_id id, face_info& out);
    [[nodiscard]] face_cache_stats stats();
    /// The directory registered with fontconfig ("" when none was, or none exists).
    /// Initializes the cache (fontconfig, FreeType) on first use, as a resolution would.
    [[nodiscard]] const char* font_dir();
    /// Registers one more directory of fonts with this cache's fontconfig configuration,
    /// beside the bundled one (tests register tests/fonts this way). Families resolved
    /// afterwards may come from it; faces already loaded stay. False when the directory does
    /// not exist or fontconfig refuses it. Takes the lock.
    [[nodiscard]] bool add_font_dir(const char* dir);

private:
    face_cache(const char* font_dir, bool is_shared);

    struct slot {
        void* ft = nullptr;
        void* hb = nullptr;
        std::uint64_t key_hash = 0;
        std::uint64_t used = 0;
        std::uint32_t id = 0;
        std::int32_t size_26_6 = 0;
        std::uint16_t units_per_em = 0;
        std::int16_t x_height_units = 0;
        std::int16_t underline_position_units = 0;
        std::int16_t underline_thickness_units = 0;
        /// Loaded by `fallback_locked`: keyed by its file, not answering family resolutions.
        bool fallback = false;
        weight weight = weight::normal;
        style style = style::normal;
        int index = 0;
        char requested[k_face_family_bytes]{};
        char family[k_face_family_bytes]{};
        char file[k_face_path_bytes]{};
    };

    bool init_locked();
    void release_locked(slot& s);
    /// The least recently used slot (or a free one).
    [[nodiscard]] unsigned victim_locked() const;
    /// Opens `file` into slot `victim` (evicting what is there). Null on failure.
    [[nodiscard]] slot* load_locked(unsigned victim, std::uint64_t key, const char* requested,
                                    const char* file, int index, const char* matched_family,
                                    weight w, style s, bool fallback);
    [[nodiscard]] slot* find_locked(face_id id);

    std::mutex mutex_{};
    void* library_ = nullptr; ///< FT_Library
    void* config_ = nullptr;  ///< FcConfig*
    bool init_attempted_ = false;
    bool is_shared_ = false;
    std::uint32_t serial_ = 0;
    std::uint64_t clock_ = 0;
    face_cache_stats stats_{};
    char requested_dir_[k_face_path_bytes]{};
    bool dir_given_ = false;
    char font_dir_[k_face_path_bytes]{};
    slot slots_[k_face_cache_size]{};
};

} // namespace lx::text

// ── Implementation ────────────────────────────────────────────────────────────

namespace {

constexpr std::uint64_t k_fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t k_fnv_prime = 1099511628211ull;

/// The family a desc names: null or empty is the UI family.
const char* family_or_default(const char* family) {
    return family && family[0] ? family : lx::text::k_ui_family;
}

/// FNV-1a 64 of the family (cut as the slot stores it), weight and style.
std::uint64_t face_key_hash(const char* family, lx::text::weight w, lx::text::style s) {
    std::uint64_t h = k_fnv_offset;
    for (unsigned i = 0; family[i] && i + 1 < lx::text::k_face_family_bytes; ++i)
        h = (h ^ static_cast<unsigned char>(family[i])) * k_fnv_prime;
    h = (h ^ (0x100u + static_cast<unsigned>(w))) * k_fnv_prime;
    h = (h ^ (0x200u + static_cast<unsigned>(s))) * k_fnv_prime;
    return h;
}

bool same_family(const char* stored, const char* family) {
    return std::strncmp(stored, family, lx::text::k_face_family_bytes - 1) == 0;
}

void copy_cut(char* dst, std::size_t cap, const char* src) {
    std::size_t n = 0;
    for (; src && src[n] && n + 1 < cap; ++n)
        dst[n] = src[n];
    dst[n] = '\0';
}

#if defined(LUMEN_HAS_TEXT)
bool is_dir(const char* path) {
    struct stat st{};
    return path && path[0] && ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/// Families tried after the one asked for, in order (fontconfig appends its own defaults
/// after these).
std::span<const char* const> fallback_families(const char* family) {
    static constexpr const char* k_ui[] = {"sans-serif"};
    static constexpr const char* k_mono[] = {"JetBrains Mono", "JetBrainsMono Nerd Font Mono",
                                             "JetBrainsMono Nerd Font", "monospace"};
    if (std::strcmp(family, lx::text::k_ui_family) == 0)
        return k_ui;
    if (std::strcmp(family, lx::text::k_mono_family) == 0)
        return k_mono;
    return {};
}
#endif

} // namespace

lx::text::face_cache& lx::text::face_cache::shared() {
    // Function-local static: destroyed at exit, so FreeType/HarfBuzz handles and fontconfig
    // are released rather than reported as leaks.
    static face_cache c{nullptr, true};
    return c;
}

lx::text::face_cache::face_cache(const char* font_dir) : face_cache(font_dir, false) {}

lx::text::face_cache::face_cache(const char* font_dir, bool is_shared) : is_shared_{is_shared} {
    if (font_dir) {
        dir_given_ = true;
        copy_cut(requested_dir_, sizeof(requested_dir_), font_dir);
    }
}

lx::text::face_cache::~face_cache() {
    std::lock_guard lock{mutex_};
    for (auto& s : slots_)
        release_locked(s);
#if defined(LUMEN_HAS_TEXT)
    if (library_) {
        FT_Done_FreeType(static_cast<FT_Library>(library_));
        library_ = nullptr;
    }
    if (config_) {
        FcConfigDestroy(static_cast<FcConfig*>(config_));
        config_ = nullptr;
        // The last cache standing (the shared one, at exit) releases fontconfig's caches.
        if (is_shared_)
            FcFini();
    }
#endif
}

bool lx::text::face_cache::init_locked() {
    if (init_attempted_)
        return library_ && config_;
    init_attempted_ = true;
#if defined(LUMEN_HAS_TEXT)
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0)
        return false;
    library_ = lib;
    FcConfig* config = FcInitLoadConfigAndFonts();
    if (!config)
        return false;
    config_ = config;

    // The bundled fonts' directory: the override, else the first of the build tree, a
    // relocated install (next to the executable) and the configured install that exists.
    char candidate[k_face_path_bytes]{};
    if (dir_given_) {
        copy_cut(candidate, sizeof(candidate), requested_dir_);
    } else if (const char* env = ::getenv("LUMEN_FONT_DIR"); env && env[0]) {
        copy_cut(candidate, sizeof(candidate), env);
    } else {
#if defined(LUMEN_FONT_DIR_BUILD)
        if (is_dir(LUMEN_FONT_DIR_BUILD))
            copy_cut(candidate, sizeof(candidate), LUMEN_FONT_DIR_BUILD);
#endif
#if defined(LUMEN_FONT_DIR_INSTALL_RELATIVE)
        if (!candidate[0]) {
            char exe[k_face_path_bytes]{};
            const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (n > 0) {
                exe[n] = '\0';
                // <prefix>/bin/<exe> → <prefix>/<datadir>/lumen/fonts
                for (int cut = 0; cut < 2; ++cut)
                    if (char* slash = std::strrchr(exe, '/'))
                        *slash = '\0';
                char rel[k_face_path_bytes]{};
                const int w = std::snprintf(rel, sizeof(rel), "%s/%s", exe,
                                            LUMEN_FONT_DIR_INSTALL_RELATIVE);
                if (w > 0 && static_cast<std::size_t>(w) < sizeof(rel) && is_dir(rel))
                    copy_cut(candidate, sizeof(candidate), rel);
            }
        }
#endif
#if defined(LUMEN_FONT_DIR_INSTALL)
        if (!candidate[0] && is_dir(LUMEN_FONT_DIR_INSTALL))
            copy_cut(candidate, sizeof(candidate), LUMEN_FONT_DIR_INSTALL);
#endif
    }
    if (is_dir(candidate) &&
        FcConfigAppFontAddDir(config, reinterpret_cast<const FcChar8*>(candidate)))
        copy_cut(font_dir_, sizeof(font_dir_), candidate);
    return true;
#else
    return false;
#endif
}

void lx::text::face_cache::release_locked(slot& s) {
#if defined(LUMEN_HAS_TEXT)
    if (s.hb)
        hb_font_destroy(static_cast<hb_font_t*>(s.hb));
    if (s.ft)
        FT_Done_Face(static_cast<FT_Face>(s.ft));
#endif
    s = slot{};
}

lx::text::face_cache::slot* lx::text::face_cache::find_locked(face_id id) {
    if (!id.valid())
        return nullptr;
    slot& s = slots_[id.value % k_face_cache_size];
    return s.hb && s.id == id.value ? &s : nullptr;
}

lx::text::face_id lx::text::face_cache::resolve(const font_desc& desc) {
    return resolve(desc.family, desc.weight, desc.style);
}

lx::text::face_id lx::text::face_cache::resolve(const char* family, weight w, style s) {
    std::lock_guard lock{mutex_};
    return resolve_locked(family, w, s);
}

unsigned lx::text::face_cache::victim_locked() const {
    unsigned victim = 0;
    for (unsigned i = 0; i < k_face_cache_size; ++i) {
        const slot& e = slots_[i];
        if (!e.hb)
            return i;
        if (e.used < slots_[victim].used)
            victim = i;
    }
    return victim;
}

lx::text::face_id lx::text::face_cache::resolve_locked(const char* family_in, weight w, style st) {
    const char* family = family_or_default(family_in);
    const std::uint64_t key = face_key_hash(family, w, st);
    for (unsigned i = 0; i < k_face_cache_size; ++i) {
        slot& e = slots_[i];
        if (e.hb && !e.fallback && e.key_hash == key && e.weight == w && e.style == st &&
            same_family(e.requested, family)) {
            e.used = ++clock_;
            ++stats_.hits;
            return {e.id};
        }
    }
#if defined(LUMEN_HAS_TEXT)
    if (!init_locked()) {
        ++stats_.failures;
        return {};
    }
    auto* config = static_cast<FcConfig*>(config_);
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(family));
    if (!pattern) {
        ++stats_.failures;
        return {};
    }
    for (const char* f : fallback_families(family))
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(f));
    FcPatternAddInteger(pattern, FC_WEIGHT, w == weight::bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, st == style::italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult match_result{};
    FcPattern* matched = FcFontMatch(config, pattern, &match_result);
    FcPatternDestroy(pattern);
    FcChar8* match_file = nullptr;
    if (!matched || match_result != FcResultMatch ||
        FcPatternGetString(matched, FC_FILE, 0, &match_file) != FcResultMatch || !match_file) {
        if (matched)
            FcPatternDestroy(matched);
        ++stats_.failures;
        return {};
    }
    int face_index = 0;
    FcPatternGetInteger(matched, FC_INDEX, 0, &face_index);
    FcChar8* match_family = nullptr;
    FcPatternGetString(matched, FC_FAMILY, 0, &match_family);
    char file[k_face_path_bytes];
    char matched_family[k_face_family_bytes];
    copy_cut(file, sizeof(file), reinterpret_cast<const char*>(match_file));
    copy_cut(matched_family, sizeof(matched_family),
             match_family ? reinterpret_cast<const char*>(match_family) : "");
    FcPatternDestroy(matched);

    const slot* e = load_locked(victim_locked(), key, family, file, face_index, matched_family, w,
                                st, false);
    if (!e) {
        ++stats_.failures;
        return {};
    }
    return {e->id};
#else
    ++stats_.failures;
    return {};
#endif
}

lx::text::face_cache::slot* lx::text::face_cache::load_locked(unsigned victim, std::uint64_t key,
                                                              const char* requested,
                                                              const char* file, int face_index,
                                                              const char* matched_family, weight w,
                                                              style st, bool fallback) {
#if defined(LUMEN_HAS_TEXT)
    FT_Face face = nullptr;
    if (FT_New_Face(static_cast<FT_Library>(library_), file, face_index, &face) != 0 || !face)
        return nullptr;
    // Sized at load only so hb_font_t starts from a valid scale; every use sets its own.
    const std::int32_t initial = size_26_6_from_pt(11.f);
    FT_Set_Char_Size(face, 0, initial, 72, 72);
    hb_font_t* font = hb_ft_font_create_referenced(face);
    if (!font) {
        FT_Done_Face(face);
        return nullptr;
    }
    // x-height in font units: OS/2 sxHeight (version 2+), else the outline of "x".
    std::int16_t x_height = 0;
    if (const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
        os2 && os2->version >= 2 && os2->version != 0xFFFFu)
        x_height = os2->sxHeight;
    if (x_height <= 0) {
        const FT_UInt gx = FT_Get_Char_Index(face, 'x');
        if (gx && FT_Load_Glyph(face, gx, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) == 0)
            x_height = static_cast<std::int16_t>(face->glyph->metrics.horiBearingY);
    }

    slot& e = slots_[victim];
    if (e.hb)
        ++stats_.evictions;
    release_locked(e);
    e.ft = face;
    e.hb = font;
    e.key_hash = key;
    e.used = ++clock_;
    // Unique id whose low bits name the slot: (serial × slots) + slot, never 0.
    ++serial_;
    e.id = serial_ * k_face_cache_size + victim;
    if (e.id == 0)
        e.id = (++serial_) * k_face_cache_size + victim;
    e.size_26_6 = initial;
    e.units_per_em = face->units_per_EM;
    e.x_height_units = x_height;
    // FreeType's underline_position is the centre of the line, negative below the baseline.
    e.underline_position_units = face->underline_position;
    e.underline_thickness_units = face->underline_thickness;
    e.fallback = fallback;
    e.weight = w;
    e.style = st;
    e.index = face_index;
    copy_cut(e.requested, sizeof(e.requested), requested);
    copy_cut(e.family, sizeof(e.family), matched_family);
    copy_cut(e.file, sizeof(e.file), file);
    ++stats_.loads;
    return &e;
#else
    (void)victim;
    (void)key;
    (void)requested;
    (void)file;
    (void)face_index;
    (void)matched_family;
    (void)w;
    (void)st;
    (void)fallback;
    return nullptr;
#endif
}

bool lx::text::face_cache::has_char_locked(face_id id, char32_t codepoint) {
    slot* s = find_locked(id);
    if (!s)
        return false;
    s->used = ++clock_;
#if defined(LUMEN_HAS_TEXT)
    return FT_Get_Char_Index(static_cast<FT_Face>(s->ft), static_cast<FT_ULong>(codepoint)) != 0;
#else
    (void)codepoint;
    return false;
#endif
}

lx::text::face_id lx::text::face_cache::fallback_locked(face_id primary, char32_t codepoint) {
    const slot* p = find_locked(primary);
    if (!p)
        return {};
#if defined(LUMEN_HAS_TEXT)
    if (!init_locked())
        return {};
    ++stats_.fallback_searches;
    // Copies: loading below may evict the primary's slot.
    char requested[k_face_family_bytes];
    copy_cut(requested, sizeof(requested), p->requested);
    char primary_file[k_face_path_bytes];
    copy_cut(primary_file, sizeof(primary_file), p->file);
    const int primary_index = p->index;
    const weight w = p->weight;
    const style st = p->style;

    auto* config = static_cast<FcConfig*>(config_);
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(requested));
    FcCharSet* chars = FcCharSetCreate();
    if (!pattern || !chars) {
        if (pattern)
            FcPatternDestroy(pattern);
        if (chars)
            FcCharSetDestroy(chars);
        ++stats_.fallback_failures;
        return {};
    }
    for (const char* f : fallback_families(requested))
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(f));
    FcCharSetAddChar(chars, static_cast<FcChar32>(codepoint));
    // The charset outranks the family in fontconfig's match order, so the match has the
    // character whenever any installed face does; the family breaks ties between those.
    FcPatternAddCharSet(pattern, FC_CHARSET, chars);
    FcCharSetDestroy(chars);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcPatternAddInteger(pattern, FC_WEIGHT, w == weight::bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, st == style::italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult match_result{};
    FcPattern* matched = FcFontMatch(config, pattern, &match_result);
    FcPatternDestroy(pattern);
    FcChar8* match_file = nullptr;
    FcCharSet* match_chars = nullptr;
    FcBool scalable = FcFalse;
    const bool usable =
        matched && match_result == FcResultMatch &&
        FcPatternGetString(matched, FC_FILE, 0, &match_file) == FcResultMatch && match_file &&
        FcPatternGetCharSet(matched, FC_CHARSET, 0, &match_chars) == FcResultMatch &&
        FcCharSetHasChar(match_chars, static_cast<FcChar32>(codepoint)) &&
        FcPatternGetBool(matched, FC_SCALABLE, 0, &scalable) == FcResultMatch && scalable;
    if (!usable) {
        if (matched)
            FcPatternDestroy(matched);
        ++stats_.fallback_failures;
        return {};
    }
    int face_index = 0;
    FcPatternGetInteger(matched, FC_INDEX, 0, &face_index);
    FcChar8* match_family = nullptr;
    FcPatternGetString(matched, FC_FAMILY, 0, &match_family);
    char file[k_face_path_bytes];
    char matched_family[k_face_family_bytes];
    copy_cut(file, sizeof(file), reinterpret_cast<const char*>(match_file));
    copy_cut(matched_family, sizeof(matched_family),
             match_family ? reinterpret_cast<const char*>(match_family) : "");
    FcPatternDestroy(matched);
    if (std::strcmp(file, primary_file) == 0 && face_index == primary_index) {
        // fontconfig's charset says the primary has it, its cmap does not: nothing better.
        ++stats_.fallback_failures;
        return {};
    }

    // Keyed by the file: a fallback slot is found by what it is, not by a family name.
    std::uint64_t key = k_fnv_offset ^ 0xFA11BACCull;
    for (const char* c = file; *c; ++c)
        key = (key ^ static_cast<unsigned char>(*c)) * k_fnv_prime;
    key = (key ^ (0x300u + static_cast<unsigned>(face_index))) * k_fnv_prime;
    key = (key ^ (0x100u + static_cast<unsigned>(w))) * k_fnv_prime;
    key = (key ^ (0x200u + static_cast<unsigned>(st))) * k_fnv_prime;
    for (unsigned i = 0; i < k_face_cache_size; ++i) {
        slot& e = slots_[i];
        if (e.hb && e.fallback && e.key_hash == key && e.index == face_index && e.weight == w &&
            e.style == st && std::strcmp(e.file, file) == 0) {
            e.used = ++clock_;
            ++stats_.hits;
            return {e.id};
        }
    }
    const slot* e = load_locked(victim_locked(), key, matched_family, file, face_index,
                                matched_family, w, st, true);
    if (!e) {
        ++stats_.fallback_failures;
        return {};
    }
    return {e->id};
#else
    (void)codepoint;
    return {};
#endif
}

lx::text::face_cache::handles lx::text::face_cache::use_locked(face_id id, std::int32_t size_26_6) {
    slot* s = find_locked(id);
    if (!s || size_26_6 <= 0)
        return {};
#if defined(LUMEN_HAS_TEXT)
    if (s->size_26_6 != size_26_6) {
        FT_Set_Char_Size(static_cast<FT_Face>(s->ft), 0, size_26_6, 72, 72);
        hb_ft_font_changed(static_cast<hb_font_t*>(s->hb));
        s->size_26_6 = size_26_6;
        ++stats_.size_changes;
    }
#endif
    s->used = ++clock_;
    return {s->ft, s->hb};
}

lx::result<lx::text::face_metrics> lx::text::face_cache::metrics_locked(face_id id,
                                                                        std::int32_t size_26_6) {
#if defined(LUMEN_HAS_TEXT)
    const handles h = use_locked(id, size_26_6);
    if (!h.ft_face)
        return lx::make_error(lx::io_err::not_found, "face not loaded");
    const slot* s = find_locked(id);
    auto* face = static_cast<FT_Face>(h.ft_face);
    const FT_Size_Metrics& m = face->size->metrics;
    face_metrics out{};
    out.units_per_em = s->units_per_em;
    out.size_26_6 = size_26_6;
    out.ascent = static_cast<std::int32_t>(m.ascender);
    out.descent = static_cast<std::int32_t>(-m.descender);
    const auto gap = static_cast<std::int32_t>(m.height) - out.ascent - out.descent;
    out.line_gap = gap > 0 ? gap : 0;
    out.x_height = static_cast<std::int32_t>(FT_MulFix(s->x_height_units, m.y_scale));
    // 26.6 em = size_26_6; a value in font units scales by size / units_per_em.
    const auto units = [&](std::int32_t v) {
        return s->units_per_em
                   ? static_cast<std::int32_t>(static_cast<std::int64_t>(v) * size_26_6 /
                                               s->units_per_em)
                   : 0;
    };
    std::int32_t thickness = units(s->underline_thickness_units);
    std::int32_t centre = -units(s->underline_position_units);
    if (s->underline_thickness_units <= 0) {
        thickness = size_26_6 / 20;
        centre = size_26_6 / 14;
    }
    if (thickness < 64)
        thickness = 64;
    out.underline_thickness = thickness;
    out.underline_offset = centre - thickness / 2;
    return out;
#else
    (void)id;
    (void)size_26_6;
    return lx::not_implemented("lx::text::face_cache::metrics");
#endif
}

lx::result<lx::text::face_metrics> lx::text::face_cache::metrics(face_id id,
                                                                 std::int32_t size_26_6) {
    std::lock_guard lock{mutex_};
    return metrics_locked(id, size_26_6);
}

bool lx::text::face_cache::alive(face_id id) {
    std::lock_guard lock{mutex_};
    return find_locked(id) != nullptr;
}

bool lx::text::face_cache::describe(face_id id, face_info& out) {
    std::lock_guard lock{mutex_};
    const slot* s = find_locked(id);
    if (!s)
        return false;
    out = {};
    copy_cut(out.requested_family, sizeof(out.requested_family), s->requested);
    copy_cut(out.family, sizeof(out.family), s->family);
    copy_cut(out.file, sizeof(out.file), s->file);
    out.index = s->index;
    out.weight = s->weight;
    out.style = s->style;
    return true;
}

lx::text::face_cache_stats lx::text::face_cache::stats() {
    std::lock_guard lock{mutex_};
    face_cache_stats out = stats_;
    out.live = 0;
    for (const auto& s : slots_)
        out.live += s.hb ? 1u : 0u;
    return out;
}

bool lx::text::face_cache::add_font_dir(const char* dir) {
    std::lock_guard lock{mutex_};
#if defined(LUMEN_HAS_TEXT)
    if (!is_dir(dir) || !init_locked())
        return false;
    return FcConfigAppFontAddDir(static_cast<FcConfig*>(config_),
                                 reinterpret_cast<const FcChar8*>(dir)) != 0;
#else
    (void)dir;
    return false;
#endif
}

const char* lx::text::face_cache::font_dir() {
    std::lock_guard lock{mutex_};
    (void)init_locked();
    return font_dir_;
}
