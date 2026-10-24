module;

#if defined(LUMEN_HAS_TEXT)
#include <hb.h>
#endif
#if defined(LUMEN_HAS_FRIBIDI)
#include <fribidi.h>
#endif
#if defined(LUMEN_HAS_UNIBREAK)
#include <graphemebreak.h>
#include <linebreak.h>
#endif

import lx.foundation;
import lx.runtime;
import lx.trace;

export module lx.text:itemize;

import std;

import :faces;
import :shape;

export namespace lx::text {

// ── Itemization (ADR-010 §4.6 `:itemize`) ────────────────────────────────────────
//
// `itemizer` splits one paragraph into runs of one (bidi level, script, face, attribute
// span), the unit `:shape` shapes. Per paragraph it decodes the UTF-8 once and analyses each
// code point:
//
// - Bidi embedding levels, UAX #9 rules P2–I2 and L1 parts 1–3 (fribidi): the paragraph
//   level from the first strong character (P2/P3; level 0 when there is none) or forced.
//   Rule L1 part 4 (trailing whitespace) and L2 (reordering) are per line: `:lines`.
// - Script (HarfBuzz's Unicode functions, the UCD Script property): Inherited takes the
//   preceding character's script, Common the preceding resolved one (a leading run of Common
//   takes the first resolved script after it), so digits and punctuation stay in their
//   word's run.
// - Grapheme cluster boundaries (UAX #29, libunibreak) and line break opportunities
//   (UAX #14, libunibreak), handed to `:lines` through `char_flags()`.
// - The face, per grapheme cluster: the primary face of its attribute span when that face
//   maps every non-ignorable code point of the cluster; otherwise a fallback found through
//   fontconfig's charset for the first missing one (`face_cache::fallback_locked`) and kept
//   in a fixed table keyed by (primary face, script, block of 128 code points), negative
//   answers included; the primary (its .notdef) when no installed face has it. A cluster
//   of Common / Inherited characters (spaces, digits, punctuation) stays in the face of the
//   run before it when that face has them, so "שלום 123" is not split into faces at the
//   space.
//
// Runs never start inside a grapheme cluster: a cluster takes the level, script, face and
// span of its first code point. Without fribidi every level is the paragraph's (0, or 1 when
// RTL is forced: "one LTR run per face"); without libunibreak clusters are a code point plus
// its following marks, joiners and variation selectors, and lines break after spaces only.
//
// Caps, fixed at construction: `max_chars` code points per call (a longer paragraph is
// itemized up to the last grapheme boundary before the cap and `bytes` says how much was
// consumed; the caller itemizes the rest as a further chunk, a new bidi paragraph), and
// `max_runs` runs. A paragraph with more runs applies `overflow`: `reject` fails the call;
// the others extend the last run over the rest of the paragraph (it is shaped with one
// face and direction) and flag `runs_overflowed`, `warn_and_drop` also warning once.
//
// Threading contract. An itemizer has one owner, like a shape cache; `set_owner_affinity`
// makes it assert. `itemize_locked` expects `faces.lock()` held (coverage and fallback use
// the faces); `shared()` is guarded by `face_cache::shared().lock()`. The spans it returns
// point into the itemizer and stay valid until its next call. fribidi allocates internally
// per call; nothing else allocates after construction.

inline constexpr unsigned k_itemize_max_chars = 65536;
inline constexpr unsigned k_itemize_max_runs = 4096;
inline constexpr unsigned k_fallback_cache_entries = 256;
/// Direct-mapped memo of (face, code point) → the face's cmap has it.
inline constexpr unsigned k_coverage_memo_entries = 4096;
inline constexpr std::uint16_t k_no_span = 0xFFFF;

/// Per code point, `char_flags()`: what holds at the position after it.
inline constexpr std::uint8_t k_char_break_allowed = 0x01;  ///< UAX #14 break opportunity
inline constexpr std::uint8_t k_char_break_mandatory = 0x02; ///< UAX #14 mandatory break
inline constexpr std::uint8_t k_char_grapheme_end = 0x04;   ///< UAX #29 cluster boundary
/// About the code point itself.
inline constexpr std::uint8_t k_char_whitespace = 0x08; ///< hangs at a line end (UAX #9 L1)
inline constexpr std::uint8_t k_char_ignorable = 0x10;  ///< not needed from a face (Cf, VS, Cc)
inline constexpr std::uint8_t k_char_neutral = 0x20;    ///< Script Common or Inherited

/// A primary face over UTF-8 bytes [begin, end) of the paragraph (an attribute span).
struct face_range {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    face_id face{};
    std::uint16_t span = k_no_span;
};

struct itemize_request {
    /// One paragraph (no paragraph separators).
    std::string_view utf8{};
    /// The primary face where no range applies.
    face_id face{};
    /// Sorted, not overlapping.
    std::span<const face_range> faces{};
    /// `automatic` (first strong character), `ltr` or `rtl`.
    text_direction direction = text_direction::automatic;
    /// BCP 47, for UAX #14 tailoring (libunibreak's "zh", "ja", "ko"); null = none.
    const char* language = nullptr;
};

struct text_run {
    /// UTF-8 bytes of the paragraph.
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    /// Code points [char_begin, char_end) in `code_points()`.
    std::uint32_t char_begin = 0;
    std::uint32_t char_end = 0;
    face_id face{};
    /// ISO 15924 tag (HarfBuzz's hb_script_t); 0 without HarfBuzz.
    std::uint32_t script = 0;
    std::uint16_t span = k_no_span;
    /// UAX #9 embedding level; odd is right-to-left.
    std::uint8_t level = 0;
    /// The face is a fallback, not the span's primary.
    bool fallback = false;

    [[nodiscard]] constexpr bool rtl() const { return (level & 1u) != 0; }
};

struct itemized_paragraph {
    /// Logical order.
    std::span<const text_run> runs{};
    /// `ltr` or `rtl`, resolved.
    text_direction direction = text_direction::ltr;
    std::uint8_t base_level = 0;
    std::uint32_t chars = 0;
    /// UTF-8 bytes itemized: less than the request's when `max_chars` cut it.
    std::uint32_t bytes = 0;
    bool runs_overflowed = false;
};

struct itemizer_config {
    unsigned max_chars = k_itemize_max_chars;
    unsigned max_runs = k_itemize_max_runs;
    lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop;
};

struct itemizer_counters {
    std::uint64_t paragraphs = 0;
    std::uint64_t runs = 0;
    /// Clusters that took a fallback face.
    std::uint64_t fallback_clusters = 0;
    /// Fallback table answers, and fontconfig searches behind its misses.
    std::uint64_t fallback_hits = 0;
    std::uint64_t fallback_searches = 0;
    /// Paragraphs whose runs hit `max_runs`, and calls cut at `max_chars`.
    std::uint64_t run_overflows = 0;
    std::uint64_t char_cuts = 0;
};

/// Built with fribidi (UAX #9 levels) / libunibreak (UAX #14, UAX #29).
[[nodiscard]] constexpr bool bidi_available() {
#if defined(LUMEN_HAS_FRIBIDI)
    return true;
#else
    return false;
#endif
}
[[nodiscard]] constexpr bool segmentation_available() {
#if defined(LUMEN_HAS_UNIBREAK)
    return true;
#else
    return false;
#endif
}

class itemizer {
public:
    /// The itemizer behind `text_engine::shared()`, guarded by `face_cache::shared().lock()`.
    [[nodiscard]] static itemizer& shared();

    explicit itemizer(itemizer_config config = {});
    ~itemizer();

    itemizer(const itemizer&) = delete;
    itemizer& operator=(const itemizer&) = delete;

    /// Errors: `invalid_argument` (a vertical direction, or `max_runs` hit with `reject`),
    /// `io` (no storage). Empty text: no runs, the forced direction or LTR.
    [[nodiscard]] lx::result<itemized_paragraph> itemize_locked(face_cache& faces,
                                                                const itemize_request& request);
    /// As `itemize_locked`, taking `faces.lock()`.
    [[nodiscard]] lx::result<itemized_paragraph> itemize(face_cache& faces,
                                                         const itemize_request& request);

    /// The last call's analysis, `chars` entries each (`char_offsets` one more: the end).
    [[nodiscard]] std::span<const char32_t> code_points() const { return {cps_, chars_}; }
    [[nodiscard]] std::span<const std::uint32_t> char_offsets() const {
        return {offsets_, chars_ ? chars_ + 1u : 0u};
    }
    [[nodiscard]] std::span<const std::uint8_t> char_flags() const { return {flags_, chars_}; }
    [[nodiscard]] std::span<const std::uint8_t> levels() const { return {levels_, chars_}; }

    [[nodiscard]] itemizer_counters counters() const { return counters_; }
    /// Forgets the fallback table (after fonts were installed, or a face cache was replaced).
    void clear_fallbacks();
    void set_owner_affinity(lx::runtime::affinity owner) { owner_ = owner; }

private:
    struct coverage_entry {
        std::uint32_t face = 0;
        char32_t cp = 0;
        bool has = false;
    };
    struct fallback_entry {
        std::uint32_t primary = 0;
        std::uint32_t script = 0;
        std::uint32_t block = 0;
        std::uint32_t face = 0;
        bool valid = false;
    };

    void release();
    [[nodiscard]] std::uint32_t decode(std::string_view utf8, std::uint32_t max_chars);
    void analyse(std::uint32_t n, text_direction direction, const char* language);
    [[nodiscard]] face_id cluster_face(face_cache& faces, face_id primary, std::uint32_t begin,
                                       std::uint32_t end, bool& fallback);
    [[nodiscard]] bool covers(face_cache& faces, face_id face, std::uint32_t begin,
                              std::uint32_t end);
    [[nodiscard]] bool has(face_cache& faces, face_id face, char32_t cp);

    itemizer_config config_{};
    char32_t* cps_ = nullptr;
    std::uint32_t* offsets_ = nullptr;
    std::uint8_t* flags_ = nullptr;
    std::uint8_t* levels_ = nullptr;
    std::uint32_t* scripts_ = nullptr;
    std::uint32_t* types_ = nullptr;  ///< FriBidiCharType
    std::uint32_t* btypes_ = nullptr; ///< FriBidiBracketType
    char* breaks_ = nullptr;          ///< libunibreak output
    text_run* runs_ = nullptr;
    std::uint32_t chars_ = 0;
    std::uint8_t base_level_ = 0;
    fallback_entry fallbacks_[k_fallback_cache_entries]{};
    coverage_entry coverage_[k_coverage_memo_entries]{};
    itemizer_counters counters_{};
    lx::runtime::affinity owner_ = lx::runtime::affinity::any;
    bool warned_ = false;
};

} // namespace lx::text

// ── Implementation ────────────────────────────────────────────────────────────

namespace {

constexpr char32_t k_replacement = 0xFFFD;

/// White_Space characters that hang at a line end (UAX #14 class SP and BA spaces, tab).
constexpr bool hangs(char32_t c) {
    return c == 0x20 || c == 0x09 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A && c != 0x2007) ||
           c == 0x205F || c == 0x3000;
}

/// Could take part in right-to-left text: a strong R / AL / AN character's block, or an
/// explicit mark, embedding or isolate. A paragraph without any (and not forced RTL) is
/// level 0 throughout, so fribidi is skipped for it.
constexpr bool bidi_relevant(char32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF) || (c >= 0x10800 && c <= 0x10FFF) ||
           (c >= 0x1E800 && c <= 0x1EFFF) || c == 0x200F || c == 0x061C ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

constexpr bool variation_selector(char32_t c) {
    return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF);
}

/// Mandatory breaks inside a paragraph (UAX #14 BK): VT, FF, NEL, LS.
constexpr bool hard_break(char32_t c) {
    return c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028;
}

#if defined(LUMEN_HAS_UNIBREAK)
void init_unibreak_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        init_linebreak();
        init_graphemebreak();
    });
}
#endif

} // namespace

lx::text::itemizer& lx::text::itemizer::shared() {
    static itemizer i{};
    return i;
}

lx::text::itemizer::itemizer(itemizer_config config) : config_{config} {
    if (config_.max_chars == 0)
        config_.max_chars = 1;
    if (config_.max_runs == 0)
        config_.max_runs = 1;
    const std::size_t n = config_.max_chars;
    // Raw storage: pages become resident only as long paragraphs touch them.
    const auto raw = [](std::size_t bytes) { return ::operator new(bytes, std::nothrow); };
    cps_ = static_cast<char32_t*>(raw(n * sizeof(char32_t)));
    offsets_ = static_cast<std::uint32_t*>(raw((n + 1) * sizeof(std::uint32_t)));
    flags_ = static_cast<std::uint8_t*>(raw(n));
    levels_ = static_cast<std::uint8_t*>(raw(n));
    scripts_ = static_cast<std::uint32_t*>(raw(n * sizeof(std::uint32_t)));
    types_ = static_cast<std::uint32_t*>(raw(n * sizeof(std::uint32_t)));
    btypes_ = static_cast<std::uint32_t*>(raw(n * sizeof(std::uint32_t)));
    breaks_ = static_cast<char*>(raw(n));
    runs_ = static_cast<text_run*>(raw(std::size_t{config_.max_runs} * sizeof(text_run)));
    if (!cps_ || !offsets_ || !flags_ || !levels_ || !scripts_ || !types_ || !btypes_ ||
        !breaks_ || !runs_)
        release(); // out of memory at construction: every call reports it
#if defined(LUMEN_HAS_UNIBREAK)
    init_unibreak_once();
#endif
}

lx::text::itemizer::~itemizer() { release(); }

void lx::text::itemizer::release() {
    ::operator delete(cps_);
    ::operator delete(offsets_);
    ::operator delete(flags_);
    ::operator delete(levels_);
    ::operator delete(scripts_);
    ::operator delete(types_);
    ::operator delete(btypes_);
    ::operator delete(breaks_);
    ::operator delete(runs_);
    cps_ = nullptr;
    offsets_ = nullptr;
    flags_ = levels_ = nullptr;
    scripts_ = types_ = btypes_ = nullptr;
    breaks_ = nullptr;
    runs_ = nullptr;
}

void lx::text::itemizer::clear_fallbacks() {
    for (auto& e : fallbacks_)
        e = {};
    for (auto& e : coverage_)
        e = {};
}

bool lx::text::itemizer::has(face_cache& faces, face_id face, char32_t cp) {
    // Face ids are never reused, so an entry for an evicted face is never asked for again
    // (and a stale id is not stored: it answers false from the face cache).
    const std::uint32_t h =
        (face.value * 0x9E3779B1u) ^ (static_cast<std::uint32_t>(cp) * 0x85EBCA77u);
    coverage_entry& e = coverage_[(h ^ (h >> 16)) % k_coverage_memo_entries];
    if (e.face == face.value && e.cp == cp && e.face != 0)
        return e.has;
    const bool r = faces.has_char_locked(face, cp);
    if (faces.alive_locked(face))
        e = {face.value, cp, r};
    return r;
}

std::uint32_t lx::text::itemizer::decode(std::string_view s, std::uint32_t max_chars) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t len = s.size();
    std::size_t i = 0;
    std::uint32_t n = 0;
    while (i < len && n < max_chars) {
        offsets_[n] = static_cast<std::uint32_t>(i);
        const unsigned char b = p[i];
        char32_t c = k_replacement;
        std::size_t used = 1;
        if (b < 0x80) {
            c = b;
        } else {
            unsigned need = 0;
            char32_t min = 0;
            if ((b & 0xE0u) == 0xC0u) {
                need = 1;
                c = b & 0x1Fu;
                min = 0x80;
            } else if ((b & 0xF0u) == 0xE0u) {
                need = 2;
                c = b & 0x0Fu;
                min = 0x800;
            } else if ((b & 0xF8u) == 0xF0u) {
                need = 3;
                c = b & 0x07u;
                min = 0x10000;
            }
            bool ok = need > 0 && i + need < len;
            for (unsigned k = 1; ok && k <= need; ++k) {
                if ((p[i + k] & 0xC0u) != 0x80u)
                    ok = false;
                else
                    c = (c << 6) | (p[i + k] & 0x3Fu);
            }
            if (ok && c >= min && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF)) {
                used = need + 1;
            } else {
                c = k_replacement; // one byte at a time through a bad sequence
                used = 1;
            }
        }
        cps_[n++] = c;
        i += used;
    }
    offsets_[n] = static_cast<std::uint32_t>(i);
    return n;
}

void lx::text::itemizer::analyse(std::uint32_t n, text_direction direction, const char* language) {
    // ── Bidi levels ──
    std::uint8_t base = direction == text_direction::rtl ? 1 : 0;
    for (std::uint32_t i = 0; i < n; ++i)
        levels_[i] = base;
#if defined(LUMEN_HAS_FRIBIDI)
    static_assert(sizeof(FriBidiChar) == sizeof(char32_t));
    static_assert(sizeof(FriBidiCharType) == sizeof(std::uint32_t));
    static_assert(sizeof(FriBidiBracketType) == sizeof(std::uint32_t));
    static_assert(sizeof(FriBidiLevel) == 1);
    // A paragraph with nothing right-to-left in it, not forced RTL, is level 0 throughout
    // (P2/P3 find no R or AL; no explicit embedding raises a level): fribidi is skipped.
    bool needs_bidi = direction == text_direction::rtl;
    for (std::uint32_t i = 0; i < n && !needs_bidi; ++i)
        needs_bidi = bidi_relevant(cps_[i]);
    if (needs_bidi) {
        const auto* str = reinterpret_cast<const FriBidiChar*>(cps_);
        auto* types = reinterpret_cast<FriBidiCharType*>(types_);
        auto* btypes = reinterpret_cast<FriBidiBracketType*>(btypes_);
        fribidi_get_bidi_types(str, static_cast<FriBidiStrIndex>(n), types);
        fribidi_get_bracket_types(str, static_cast<FriBidiStrIndex>(n), types, btypes);
        FriBidiParType dir = direction == text_direction::rtl   ? FRIBIDI_PAR_RTL
                             : direction == text_direction::ltr ? FRIBIDI_PAR_LTR
                                                                : FRIBIDI_PAR_ON;
        const FriBidiLevel max = fribidi_get_par_embedding_levels_ex(
            types, btypes, static_cast<FriBidiStrIndex>(n), &dir,
            reinterpret_cast<FriBidiLevel*>(levels_));
        if (max > 0) {
            base = FRIBIDI_DIR_TO_LEVEL(dir);
        } else {
            // fribidi failed (out of memory): one level, as without it.
            for (std::uint32_t i = 0; i < n; ++i)
                levels_[i] = base;
        }
    }
#endif
    base_level_ = base;

    // ── Scripts and per-character properties ──
#if defined(LUMEN_HAS_TEXT)
    hb_unicode_funcs_t* uf = hb_unicode_funcs_get_default();
#endif
    std::uint32_t last = 0; // the last resolved real script
    std::uint32_t first_unresolved = 0;
    bool any_resolved = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        const char32_t c = cps_[i];
        std::uint8_t f = 0;
        if (hangs(c))
            f |= k_char_whitespace;
        std::uint32_t sc = 0;
#if defined(LUMEN_HAS_TEXT)
        const hb_unicode_general_category_t gc = hb_unicode_general_category(uf, c);
        if (gc == HB_UNICODE_GENERAL_CATEGORY_FORMAT || gc == HB_UNICODE_GENERAL_CATEGORY_CONTROL ||
            variation_selector(c))
            f |= k_char_ignorable;
        const hb_script_t raw = hb_unicode_script(uf, c);
        if (raw == HB_SCRIPT_INHERITED || raw == HB_SCRIPT_COMMON || raw == HB_SCRIPT_UNKNOWN) {
            sc = any_resolved ? last : 0;
            if (raw != HB_SCRIPT_UNKNOWN)
                f |= k_char_neutral;
        } else {
            sc = static_cast<std::uint32_t>(raw);
            if (!any_resolved) {
                // A leading Common/Inherited stretch takes the first real script.
                for (std::uint32_t k = first_unresolved; k < i; ++k)
                    scripts_[k] = sc;
                any_resolved = true;
            }
            last = sc;
        }
#else
        if (c < 0x20 || variation_selector(c) || c == 0x200D || c == 0x200C)
            f |= k_char_ignorable;
#endif
        scripts_[i] = sc;
        flags_[i] = f;
    }
#if defined(LUMEN_HAS_TEXT)
    if (!any_resolved)
        for (std::uint32_t k = 0; k < n; ++k)
            scripts_[k] = static_cast<std::uint32_t>(HB_SCRIPT_COMMON);
#endif

    // ── Grapheme clusters (UAX #29) and line breaks (UAX #14) ──
#if defined(LUMEN_HAS_UNIBREAK)
    static_assert(sizeof(utf32_t) == sizeof(char32_t));
    const auto* u32 = reinterpret_cast<const utf32_t*>(cps_);
    set_graphemebreaks_utf32(u32, n, language, breaks_);
    for (std::uint32_t i = 0; i < n; ++i)
        if (breaks_[i] == GRAPHEMEBREAK_BREAK)
            flags_[i] |= k_char_grapheme_end;
    set_linebreaks_utf32(u32, n, language, breaks_);
    for (std::uint32_t i = 0; i + 1 < n; ++i) {
        if (breaks_[i] == LINEBREAK_MUSTBREAK)
            flags_[i] |= k_char_break_mandatory | k_char_break_allowed;
        else if (breaks_[i] == LINEBREAK_ALLOWBREAK)
            flags_[i] |= k_char_break_allowed;
    }
#else
    (void)language;
    for (std::uint32_t i = 0; i < n; ++i) {
        const char32_t c = cps_[i];
        const char32_t next = i + 1 < n ? cps_[i + 1] : 0;
        bool joins = false;
        if (i + 1 < n) {
#if defined(LUMEN_HAS_TEXT)
            const auto gc = hb_unicode_general_category(uf, next);
            joins = gc == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
                    gc == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
                    gc == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
#endif
            joins = joins || next == 0x200D || c == 0x200D || variation_selector(next) ||
                    (next >= 0x1F3FB && next <= 0x1F3FF) || (c == 0x0D && next == 0x0A);
        }
        if (!joins)
            flags_[i] |= k_char_grapheme_end;
        if (i + 1 < n) {
            if (hard_break(c))
                flags_[i] |= k_char_break_mandatory | k_char_break_allowed;
            else if (hangs(c) && !hangs(next) && !joins)
                flags_[i] |= k_char_break_allowed;
        }
    }
#endif
    if (n > 0)
        flags_[n - 1] |= k_char_grapheme_end;
}

bool lx::text::itemizer::covers(face_cache& faces, face_id face, std::uint32_t begin,
                                std::uint32_t end) {
    for (std::uint32_t i = begin; i < end; ++i)
        if (!(flags_[i] & k_char_ignorable) && !has(faces, face, cps_[i]))
            return false;
    return true;
}

lx::text::face_id lx::text::itemizer::cluster_face(face_cache& faces, face_id primary,
                                                   std::uint32_t begin, std::uint32_t end,
                                                   bool& fallback) {
    fallback = false;
    std::uint32_t missing = end;
    for (std::uint32_t i = begin; i < end; ++i)
        if (!(flags_[i] & k_char_ignorable) && !has(faces, primary, cps_[i])) {
            missing = i;
            break;
        }
    if (missing == end)
        return primary;
    const char32_t c = cps_[missing];
    const std::uint32_t script = scripts_[begin];
    const std::uint32_t block = static_cast<std::uint32_t>(c) >> 7;
    std::uint64_t h = (static_cast<std::uint64_t>(primary.value) * 0x9E3779B97F4A7C15ull) ^
                      (static_cast<std::uint64_t>(script) * 0xC2B2AE3D27D4EB4Full) ^ block;
    h ^= h >> 29;
    fallback_entry& e = fallbacks_[h % k_fallback_cache_entries];
    face_id fb{};
    if (e.valid && e.primary == primary.value && e.script == script && e.block == block &&
        (e.face == 0 || has(faces, face_id{e.face}, c))) {
        // A live answer for the block (or "none"). A face from the table that lacks this
        // character falls through to a fresh search for it.
        ++counters_.fallback_hits;
        fb = face_id{e.face};
    } else {
        ++counters_.fallback_searches;
        fb = faces.fallback_locked(primary, c);
        e = {primary.value, script, block, fb.value, true};
    }
    if (!fb.valid())
        return primary;
    if (covers(faces, fb, begin, end) || has(faces, fb, cps_[begin])) {
        fallback = true;
        ++counters_.fallback_clusters;
        return fb;
    }
    return primary;
}

lx::result<lx::text::itemized_paragraph>
lx::text::itemizer::itemize(face_cache& faces, const itemize_request& request) {
    auto lock = faces.lock();
    return itemize_locked(faces, request);
}

lx::result<lx::text::itemized_paragraph>
lx::text::itemizer::itemize_locked(face_cache& faces, const itemize_request& request) {
    lx::runtime::assert_affinity(owner_);
    if (request.direction == text_direction::ttb || request.direction == text_direction::btt)
        return lx::invalid_argument("itemize: vertical text is not supported");
    if (!cps_)
        return lx::make_error(lx::io_err::not_found, "itemize: no storage");
    if (request.utf8.size() >= 0xFFFFFFFFu)
        return lx::invalid_argument("itemize: paragraph too long");
    chars_ = 0;
    itemized_paragraph out{};
    if (request.utf8.empty()) {
        // An empty paragraph still has a direction: the forced one, else LTR.
        out.base_level = request.direction == text_direction::rtl ? 1 : 0;
        out.direction = out.base_level ? text_direction::rtl : text_direction::ltr;
        ++counters_.paragraphs;
        return out;
    }

    std::uint32_t n = decode(request.utf8, config_.max_chars);
    analyse(n, request.direction, request.language);
    if (offsets_[n] < request.utf8.size()) {
        // Cut at the cap: end on the last grapheme boundary strictly inside, so the cluster
        // the cap fell in starts the next chunk whole; analyse the shorter text again.
        ++counters_.char_cuts;
        std::uint32_t k = n - 1;
        while (k > 0 && !(flags_[k - 1] & k_char_grapheme_end))
            --k;
        if (k > 0) {
            n = k;
            analyse(n, request.direction, request.language);
        }
    }
    chars_ = n;

    // ── Runs, one per change of (level, script, face, span), at cluster starts ──
    std::uint32_t nruns = 0;
    const face_range* range = request.faces.data();
    const face_range* range_end = range + request.faces.size();
    bool overflowed = false;
    for (std::uint32_t i = 0; i < n;) {
        std::uint32_t j = i + 1;
        while (j < n && !(flags_[j - 1] & k_char_grapheme_end))
            ++j;
        const std::uint32_t byte = offsets_[i];
        while (range != range_end && range->end <= byte)
            ++range;
        face_id primary = request.face;
        std::uint16_t span = k_no_span;
        if (range != range_end && range->begin <= byte) {
            primary = range->face.valid() ? range->face : request.face;
            span = range->span;
        }
        text_run* cur = nruns ? &runs_[nruns - 1] : nullptr;
        bool neutral = true;
        for (std::uint32_t k = i; k < j && neutral; ++k)
            neutral = (flags_[k] & (k_char_neutral | k_char_ignorable)) != 0;
        bool fallback = false;
        face_id face{};
        if (neutral && cur && cur->span == span && cur->face != primary &&
            covers(faces, cur->face, i, j)) {
            face = cur->face; // a space or digit between fallback text keeps its face
            fallback = cur->fallback;
        } else {
            face = cluster_face(faces, primary, i, j, fallback);
        }
        const std::uint8_t level = levels_[i];
        const std::uint32_t script = scripts_[i];
        if (cur && (overflowed || (cur->level == level && cur->script == script &&
                                   cur->face == face && cur->span == span))) {
            cur->end = offsets_[j];
            cur->char_end = j;
        } else if (nruns < config_.max_runs) {
            runs_[nruns++] = {offsets_[i], offsets_[j], i, j, face, script, span, level, fallback};
        } else {
            if (config_.overflow == lx::runtime::overflow_action::reject) {
                ++counters_.run_overflows;
                return lx::invalid_argument("itemize: too many runs in one paragraph");
            }
            overflowed = true;
            ++counters_.run_overflows;
            if (config_.overflow == lx::runtime::overflow_action::warn_and_drop && !warned_) {
                warned_ = true;
                lx::trace::logger::global().log(
                    lx::trace::level::warn, "text",
                    "itemize: run cap reached; the rest of a paragraph is one run");
            }
            cur->end = offsets_[j];
            cur->char_end = j;
        }
        i = j;
    }
    counters_.runs += nruns;
    ++counters_.paragraphs;
    out.runs = {runs_, nruns};
    out.base_level = base_level_;
    out.direction = base_level_ & 1u ? text_direction::rtl : text_direction::ltr;
    out.chars = n;
    out.bytes = offsets_[n];
    out.runs_overflowed = overflowed;
    return out;
}
