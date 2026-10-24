export module lx.text:lines;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;

import :faces;
import :shape;
import :itemize;

export namespace lx::text {

// ── Lines and the retained layout (ADR-010 §4.6 `:lines`) ────────────────────────
//
// `text_layout` is what a text node holds: its text, attribute spans and paragraph style,
// and the result — paragraphs → lines → positioned runs of glyphs — kept until one of its
// inputs changes. Two passes, each run only when its inputs changed (a setter given the value
// it already has changes nothing):
//
// - Shape pass (text, font, scale, attributes, direction, language, limits, engine): split
//   into paragraphs at LF, CR LF, CR and U+2029; itemize each (`:itemize`: bidi levels,
//   scripts, fallback faces, UAX #14 / #29 boundaries); shape every run once, whole, through
//   the engine's `shape_cache` (`:shape`), and keep its glyphs and their clusters — HarfBuzz
//   clusters merged until each ends on a grapheme boundary, so a cluster is never split.
// - Wrap pass (max width, wrap, max lines, ellipsis, alignment, line height): greedy line
//   breaking over the clusters at UAX #14 opportunities (a cluster boundary inside a word
//   when one cluster alone is wider than the line), trailing whitespace hanging past the
//   edge (UAX #9 L1), "…" at the logical end of the last line when `max_lines` cuts the text
//   or an unwrapped line is too wide, then each line's runs reordered visually (UAX #9 L2:
//   from the highest level to the lowest odd one, reverse every maximal sequence at or above
//   it), positioned, aligned (start / end follow the paragraph's direction) and stacked.
//   No shaping: a line cuts a run's glyphs at a cluster boundary rather than reshaping the
//   two halves, so a re-wrap at any width reuses the shape pass as it is. HarfBuzz's
//   unsafe-to-break is not consulted: breaks fall after spaces or between ideographs, where
//   no joining or kerning crosses; a forced break inside a word or an ellipsis cut keeps the
//   glyph forms the whole word was shaped with. The ellipsis is shaped once per face
//   (through the shape cache) and kept.
//
// Units: 26.6 fixed point device pixels (logical × scale), x from the layout's left edge and
// y down from its top. `max_width` is in logical pixels; the font's size in points (96 dpi)
// is multiplied by the scale.
//
// Storage, owned by the layout: vectors sized to the text that keep their capacity across
// passes (a re-wrap or a reshape of text no longer than before allocates nothing), each
// under a cap of `text_layout_limits` — UTF-8 bytes (text beyond is cut at a character
// boundary), shaped glyphs, shaped runs, lines and attribute spans (the rest are dropped).
// A cap applies the limits' `overflow`: `reject` makes the pass fail (the layout is empty
// and `update` returns the error); the others keep what fits and set `truncated()`,
// `warn_and_drop` warning once per layout. Per glyph about 20 B, per cluster 28 B, per line
// 64 B, per run 64 B, plus one byte per text byte.
//
// Hit testing: `hit_test` maps a point to a logical position and its affinity (`upstream`:
// the caret sits on the trailing edge of the cluster before the position, not the leading
// edge of the one after — the two differ at a direction boundary and a soft line break);
// `caret` maps a position and affinity back to x, so caret(hit_test(x)) is x. Inside a
// cluster of several graphemes (a ligature) carets are spaced evenly.
//
// Threading contract. A layout has one owner (the node holding it). Its queries are const
// and lay out lazily: the first query after a change runs the passes, under the engine's
// `faces.lock()` (which guards the shared shape cache and itemizer of `text_engine::shared()`),
// so it must not be called with that lock held.

enum class text_align : std::uint8_t { start, center, end };
enum class text_wrap : std::uint8_t { word, none };
enum class text_underline : std::uint8_t { none, single, wavy };
enum class span_role : std::uint8_t { none, link, spell };

/// A styled stretch of the layout's UTF-8 bytes [begin, end).
struct text_attr {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    weight weight = weight::normal;
    style style = style::normal;
    /// sRGB 0xRRGGBBAA, straight alpha; 0 is the node's text colour.
    std::uint32_t color = 0;
    text_underline underline = text_underline::none;
    span_role role = span_role::none;
    /// The caller's (a link number).
    std::uint16_t tag = 0;
    friend constexpr bool operator==(const text_attr&, const text_attr&) = default;
};

inline constexpr std::uint32_t k_layout_max_bytes = 4u << 20;
inline constexpr std::uint32_t k_layout_max_glyphs = 1u << 20;
inline constexpr std::uint32_t k_layout_max_runs = 1u << 16;
inline constexpr std::uint32_t k_layout_max_lines = 1u << 16;
inline constexpr std::uint32_t k_layout_max_attrs = 4096;
/// Faces with metrics per layout (primaries and fallbacks); more use the base face's.
inline constexpr unsigned k_layout_max_faces = 16;
/// A run longer than this is shaped in pieces cut at a break opportunity (else a grapheme
/// boundary), so no shaped run exceeds the shape cache's `max_run_glyphs`.
inline constexpr std::uint32_t k_layout_shape_piece_chars = 1024;
inline constexpr std::uint16_t k_no_attr = k_no_span;

struct text_layout_limits {
    std::uint32_t max_bytes = k_layout_max_bytes;
    std::uint32_t max_glyphs = k_layout_max_glyphs;
    std::uint32_t max_runs = k_layout_max_runs;
    std::uint32_t max_lines = k_layout_max_lines;
    std::uint32_t max_attrs = k_layout_max_attrs;
    lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop;
    friend constexpr bool operator==(const text_layout_limits&,
                                     const text_layout_limits&) = default;
};

/// One glyph, 26.6. `cluster` is the byte offset of its cluster in the layout's text (an
/// ellipsis glyph's is the offset of the cut).
struct layout_glyph {
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0;
    std::int32_t x_advance = 0;
    std::int32_t x_offset = 0;
    std::int32_t y_offset = 0;
};
static_assert(sizeof(layout_glyph) == 20);

/// The unit of wrapping and hit testing: glyphs that belong to whole graphemes [begin, end),
/// logical order within its paragraph.
struct layout_cluster {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::int32_t advance = 0;
    /// Contiguous in the glyph array, in visual order.
    std::uint32_t glyph_begin = 0;
    std::uint32_t glyph_count = 0;
    /// The shaped run.
    std::uint32_t item = 0;
    std::uint16_t graphemes = 1;
    /// Whitespace only: hangs at a line end.
    bool space = false;
};

/// A run's part on one line, in the line's visual order.
struct layout_run {
    /// Logical UTF-8 bytes (an ellipsis: begin == end, the cut).
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    /// `glyphs(run)`, visual order.
    std::uint32_t glyph_begin = 0;
    std::uint32_t glyph_count = 0;
    /// Into `clusters()`, logical order (none for an ellipsis).
    std::uint32_t cluster_begin = 0;
    std::uint32_t cluster_count = 0;
    /// Pen x of the first glyph from the layout's left edge, alignment applied; and width.
    std::int32_t x = 0;
    std::int32_t width = 0;
    face_id face{};
    std::int32_t size_26_6 = 0;
    std::uint32_t script = 0;
    /// Index into `attrs()`, or `k_no_attr`.
    std::uint16_t attr = k_no_attr;
    /// The level it was reordered at (a hanging or ellipsis run: the paragraph's).
    std::uint8_t level = 0;
    /// Glyph order is right-to-left (the run was shaped RTL).
    bool rtl = false;
    bool ellipsis = false;
    /// Trailing whitespace past the line's width (UAX #9 L1).
    bool hanging = false;
    bool fallback = false;
};

struct layout_line {
    /// Logical bytes, trailing whitespace included; an ellipsized line ends at its cut.
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    /// Into `runs()`, visual order.
    std::uint32_t run_begin = 0;
    std::uint32_t run_count = 0;
    std::uint32_t paragraph = 0;
    /// The content's left edge and width (the ellipsis in, hanging whitespace out).
    std::int32_t x = 0;
    std::int32_t width = 0;
    /// The line box from the layout's top, and the baseline in it (absolute).
    std::int32_t top = 0;
    std::int32_t height = 0;
    std::int32_t baseline = 0;
    std::int32_t ascent = 0;
    std::int32_t descent = 0;
    /// The paragraph's level (odd: right-to-left).
    std::uint8_t level = 0;
    bool ellipsized = false;
    /// The paragraph's last line.
    bool paragraph_end = false;

    [[nodiscard]] constexpr bool rtl() const { return (level & 1u) != 0; }
};

/// A logical position under a point. `upstream`: on the trailing edge of the cluster that
/// ends at `index` (pass it back to `caret`).
struct text_hit {
    std::uint32_t index = 0;
    bool upstream = false;
    std::uint32_t line = 0;
    /// The point is over the line's content.
    bool inside = false;
};

struct caret_pos {
    std::uint32_t line = 0;
    std::int32_t x = 0;
    std::int32_t top = 0;
    std::int32_t height = 0;
    /// The direction of the cluster the caret is attached to.
    bool rtl = false;
};

struct text_layout_counters {
    /// Itemize + shape of the whole text.
    std::uint64_t shape_passes = 0;
    /// Line breaking, reordering and positioning.
    std::uint64_t wrap_passes = 0;
    /// Runs sent to the shape cache, and those it had to shape (misses).
    std::uint64_t shape_requests = 0;
    std::uint64_t shape_misses = 0;
    /// Ellipses shaped (one per face per shape pass, through the cache).
    std::uint64_t ellipsis_shapes = 0;
};

/// The caches a layout works with. Null members are the shared ones.
struct text_engine {
    face_cache* faces = nullptr;
    shape_cache* shapes = nullptr;
    itemizer* items = nullptr;

    /// `face_cache::shared()`, `shape_cache::shared()`, `itemizer::shared()`: guarded by the
    /// face cache's lock.
    [[nodiscard]] static text_engine shared() {
        return {&face_cache::shared(), &shape_cache::shared(), &itemizer::shared()};
    }
    friend constexpr bool operator==(const text_engine&, const text_engine&) = default;
};

[[nodiscard]] constexpr std::int32_t to_26_6(float px) {
    return static_cast<std::int32_t>(px * 64.f + (px >= 0.f ? 0.5f : -0.5f));
}
[[nodiscard]] constexpr float from_26_6(std::int32_t v) { return static_cast<float>(v) / 64.f; }

class text_layout {
public:
    text_layout() = default;
    explicit text_layout(text_engine engine) : engine_{engine} {}

    /// Copied. Null is "".
    void set_text(const char* utf8);
    void set_text(std::string_view utf8);
    /// The family is copied (at most `k_face_family_bytes` - 1 bytes).
    void set_font(font_desc desc);
    /// Logical pixels; 0 (or less) is unbounded.
    void set_max_width(float px);
    void set_scale(float scale);
    /// Kept sorted by `begin`; an overlap is clipped to start where the previous span ends.
    void set_attrs(std::span<const text_attr> attrs);
    /// The base direction of every paragraph: `automatic` (UAX #9 P2/P3), `ltr`, `rtl`.
    void set_direction(text_direction direction);
    /// BCP 47 (at most 15 bytes kept), for shaping and UAX #14 tailoring; null is none.
    void set_language(const char* bcp47);
    void set_align(text_align align);
    void set_wrap(text_wrap wrap);
    /// 0 is unlimited.
    void set_max_lines(unsigned lines);
    /// "…" when `max_lines` cuts the text or an unwrapped line is wider than the width.
    void set_ellipsize(bool on);
    /// Line height as a multiple of the font size (ADR §5.4: φ = 1.618 for UI text); 0 is the
    /// faces' own (ascent + descent + line gap).
    void set_line_height(float multiple);
    /// Text and attributes already set beyond the new caps are cut now.
    void set_limits(text_layout_limits limits);
    void set_engine(text_engine engine);

    /// Runs the passes whose inputs changed. Queries call it themselves; this reports errors
    /// (`io` when no face resolves is not one: the layout then estimates widths, as
    /// `font_stack` does without fonts — `has_faces()` is false).
    [[nodiscard]] lx::result<void> update();
    [[nodiscard]] bool dirty() const { return shape_dirty_ || wrap_dirty_; }

    /// The layout's size in whole device pixels (rounded up); the width is at most the max
    /// width (an unbreakable word may overflow it).
    [[nodiscard]] lx::size2i bounding_size() const;
    [[nodiscard]] std::string_view text() const { return {text_.data(), text_.size()}; }
    [[nodiscard]] std::span<const text_attr> attrs() const { return attrs_; }
    [[nodiscard]] std::span<const layout_line> lines() const;
    /// Every line's runs; a line's are `runs(line)`.
    [[nodiscard]] std::span<const layout_run> runs() const;
    [[nodiscard]] std::span<const layout_run> runs(const layout_line& line) const;
    [[nodiscard]] std::span<const layout_glyph> glyphs(const layout_run& run) const;
    [[nodiscard]] std::span<const layout_cluster> clusters() const;
    /// Metrics of a face this layout uses, at its size; null for another.
    [[nodiscard]] const face_metrics* metrics(face_id face) const;
    [[nodiscard]] face_id base_face() const;
    [[nodiscard]] std::int32_t size_26_6() const;
    /// 26.6: the widest line's content, the stacked line boxes, the first baseline.
    [[nodiscard]] std::int32_t width() const;
    [[nodiscard]] std::int32_t height() const;
    [[nodiscard]] std::int32_t first_baseline() const;
    /// The first paragraph's resolved direction.
    [[nodiscard]] text_direction direction() const;
    /// A cap of `text_layout_limits` cut the text.
    [[nodiscard]] bool truncated() const;
    /// The last line ends in "…".
    [[nodiscard]] bool ellipsized() const;
    /// A real face backs the layout (else widths are estimates and there are no runs).
    [[nodiscard]] bool has_faces() const;
    [[nodiscard]] text_layout_counters counters() const { return counters_; }
    /// Capacity of the layout's storage.
    [[nodiscard]] std::size_t storage_bytes() const;

    /// x, y: 26.6 from the layout's top left.
    [[nodiscard]] text_hit hit_test(std::int32_t x, std::int32_t y) const;
    [[nodiscard]] caret_pos caret(std::uint32_t index, bool upstream = false) const;
    /// The next / previous grapheme boundary (clamped to the text).
    [[nodiscard]] std::uint32_t next_caret(std::uint32_t index) const;
    [[nodiscard]] std::uint32_t prev_caret(std::uint32_t index) const;
    [[nodiscard]] bool is_caret_stop(std::uint32_t index) const;
    /// A UAX #14 opportunity (or a paragraph boundary) at byte `index`.
    [[nodiscard]] bool break_opportunity(std::uint32_t index) const;

private:
    struct paragraph {
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        std::uint32_t cluster_begin = 0;
        std::uint32_t cluster_end = 0;
        std::uint8_t level = 0;
    };
    struct item {
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        std::uint32_t glyph_begin = 0;
        std::uint32_t glyph_count = 0;
        face_id face{};
        std::uint32_t script = 0;
        std::uint16_t attr = k_no_attr;
        std::uint8_t level = 0;
        std::uint8_t face_slot = 0;
        bool fallback = false;
    };
    struct face_entry {
        face_id id{};
        face_metrics m{};
        bool ellipsis_ready = false;
        std::uint32_t ellipsis_begin = 0;
        std::uint32_t ellipsis_count = 0;
        std::int32_t ellipsis_advance = 0;
    };

    [[nodiscard]] lx::result<void> refresh() const;
    [[nodiscard]] lx::result<void> shape_pass() const;
    [[nodiscard]] lx::result<void> wrap_pass() const;
    [[nodiscard]] bool over_cap(bool hit) const;
    [[nodiscard]] std::uint8_t face_slot_locked(face_cache& faces, face_id id) const;
    /// False: a cap was reached (with an overflow action that keeps what fits).
    [[nodiscard]] lx::result<bool> shape_item(face_cache& faces, shape_cache& shapes,
                                              std::uint32_t begin, std::uint32_t end,
                                              const text_run& run) const;
    [[nodiscard]] bool ensure_ellipsis(std::uint8_t slot) const;
    void build_line(const paragraph& p, std::uint32_t c, std::uint32_t content_end,
                    std::uint32_t hang_end, bool ellipsis, std::uint8_t ellipsis_slot,
                    std::uint32_t line_end) const;
    [[nodiscard]] std::uint32_t line_of(std::uint32_t index, bool upstream) const;
    [[nodiscard]] std::uint32_t grapheme_boundary(const layout_cluster& c, unsigned n) const;
    [[nodiscard]] unsigned graphemes_before(const layout_cluster& c, std::uint32_t index) const;
    [[nodiscard]] const face_metrics* metrics_of(face_id face) const;
    [[nodiscard]] text_engine engine() const;
    [[nodiscard]] font_desc font() const;

    // Inputs.
    std::vector<char> text_{};
    std::vector<text_attr> attrs_{};
    char family_[k_face_family_bytes]{};
    bool family_default_ = true;
    float size_pt_ = 11.f;
    weight weight_ = weight::normal;
    style style_ = style::normal;
    float max_width_ = 0.f;
    float scale_ = 1.f;
    text_direction direction_ = text_direction::automatic;
    char language_[16]{};
    text_align align_ = text_align::start;
    text_wrap wrap_ = text_wrap::word;
    unsigned max_lines_ = 0;
    bool ellipsize_ = false;
    float line_height_ = 0.f;
    text_layout_limits limits_{};
    text_engine engine_{};
    bool input_cut_ = false; ///< set_text / set_attrs dropped input over a cap

    // Results (lazily computed by const queries).
    mutable bool shape_dirty_ = true;
    mutable bool wrap_dirty_ = true;
    mutable lx::error error_{};
    mutable std::vector<std::uint8_t> pos_{};
    mutable std::vector<paragraph> paragraphs_{};
    mutable std::vector<item> items_{};
    mutable std::vector<layout_glyph> glyphs_{};
    mutable std::vector<layout_cluster> clusters_{};
    mutable std::vector<face_range> ranges_{};
    mutable std::vector<face_entry> faces_{};
    mutable std::vector<layout_glyph> ellipsis_glyphs_{};
    mutable std::vector<layout_line> lines_{};
    mutable std::vector<layout_run> line_runs_{};
    mutable face_id base_face_{};
    mutable std::int32_t size_26_6_ = 0;
    mutable std::int32_t width_ = 0;
    mutable std::int32_t height_ = 0;
    mutable bool estimate_ = false;
    mutable bool shape_cut_ = false;
    mutable bool lines_cut_ = false;
    mutable bool ellipsized_ = false;
    mutable bool warned_ = false;
    mutable text_layout_counters counters_{};
};

} // namespace lx::text

// ── Implementation ────────────────────────────────────────────────────────────

namespace {

// Position flags, one per text byte plus the end: what holds at the boundary before it.
constexpr std::uint8_t k_pos_char = 0x01;      ///< a code point starts here (or the end)
constexpr std::uint8_t k_pos_grapheme = 0x02;  ///< a grapheme cluster boundary: a caret stop
constexpr std::uint8_t k_pos_break = 0x04;     ///< a line break opportunity
constexpr std::uint8_t k_pos_mandatory = 0x08; ///< a mandatory break (LS, VT, FF, NEL)
constexpr std::uint8_t k_pos_space = 0x10;     ///< the code point starting here hangs

constexpr std::uint32_t k_npos = 0xFFFFFFFFu;
constexpr char32_t k_ellipsis = 0x2026;

/// Length of the paragraph separator at `i`: LF, CR, CR LF, U+2029; 0 when none.
std::uint32_t separator_at(const char* s, std::uint32_t i, std::uint32_t n) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b == '\n')
        return 1;
    if (b == '\r')
        return i + 1 < n && s[i + 1] == '\n' ? 2 : 1;
    if (b == 0xE2 && i + 2 < n && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        static_cast<unsigned char>(s[i + 2]) == 0xA9)
        return 3;
    return 0;
}

std::int32_t round_px(std::int32_t v) { return (v + 32) & ~63; }

} // namespace

// ── Inputs ──

lx::text::text_engine lx::text::text_layout::engine() const {
    text_engine e = engine_;
    if (!e.faces)
        e.faces = &face_cache::shared();
    if (!e.shapes)
        e.shapes = &shape_cache::shared();
    if (!e.items)
        e.items = &itemizer::shared();
    return e;
}

lx::text::font_desc lx::text::text_layout::font() const {
    font_desc d{};
    d.family = family_default_ ? k_ui_family : family_;
    d.size_pt = size_pt_;
    d.weight = weight_;
    d.style = style_;
    return d;
}

void lx::text::text_layout::set_text(const char* utf8) {
    set_text(std::string_view{utf8 ? utf8 : ""});
}

void lx::text::text_layout::set_text(std::string_view utf8) {
    std::size_t n = utf8.size();
    bool cut = false;
    if (n > limits_.max_bytes) {
        // Cut at a code point boundary at or below the cap.
        n = limits_.max_bytes;
        while (n > 0 && (static_cast<unsigned char>(utf8[n]) & 0xC0u) == 0x80u)
            --n;
        cut = true;
    }
    if (!cut && !input_cut_ && n == text_.size() &&
        (n == 0 || std::memcmp(text_.data(), utf8.data(), n) == 0))
        return;
    text_.assign(utf8.data(), utf8.data() + n);
    input_cut_ = cut;
    shape_dirty_ = wrap_dirty_ = true;
}

void lx::text::text_layout::set_font(font_desc desc) {
    const bool is_default = !desc.family || !desc.family[0];
    const char* family = is_default ? "" : desc.family;
    const bool same = is_default == family_default_ &&
                      (is_default || std::strncmp(family_, family, sizeof(family_) - 1) == 0) &&
                      desc.size_pt == size_pt_ && desc.weight == weight_ && desc.style == style_;
    if (same)
        return;
    family_default_ = is_default;
    std::size_t i = 0;
    for (; family[i] && i + 1 < sizeof(family_); ++i)
        family_[i] = family[i];
    family_[i] = '\0';
    size_pt_ = desc.size_pt;
    weight_ = desc.weight;
    style_ = desc.style;
    shape_dirty_ = wrap_dirty_ = true;
}

void lx::text::text_layout::set_max_width(float px) {
    if (!(px > 0.f))
        px = 0.f;
    if (px == max_width_)
        return;
    const std::int32_t before = to_26_6(max_width_ * scale_);
    max_width_ = px;
    if (to_26_6(max_width_ * scale_) != before)
        wrap_dirty_ = true;
}

void lx::text::text_layout::set_scale(float scale) {
    if (!(scale > 0.f))
        scale = 1.f;
    if (scale == scale_)
        return;
    scale_ = scale;
    shape_dirty_ = wrap_dirty_ = true;
}

void lx::text::text_layout::set_attrs(std::span<const text_attr> attrs) {
    // Normalize into a scratch copy first: equal input after sorting is no change.
    std::size_t n = attrs.size();
    bool cut = false;
    if (n > limits_.max_attrs) {
        n = limits_.max_attrs;
        cut = true;
    }
    std::vector<text_attr> sorted(attrs.begin(), attrs.begin() + static_cast<std::ptrdiff_t>(n));
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const text_attr& a, const text_attr& b) { return a.begin < b.begin; });
    std::uint32_t prev_end = 0;
    std::size_t out = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        text_attr a = sorted[i];
        if (a.begin < prev_end)
            a.begin = prev_end;
        if (a.end <= a.begin)
            continue;
        prev_end = a.end;
        sorted[out++] = a;
    }
    sorted.resize(out);
    if (!cut && !input_cut_ && sorted == attrs_)
        return;
    attrs_ = std::move(sorted);
    input_cut_ = input_cut_ || cut;
    shape_dirty_ = wrap_dirty_ = true;
}

void lx::text::text_layout::set_direction(text_direction direction) {
    if (direction == text_direction::ttb || direction == text_direction::btt)
        direction = text_direction::automatic;
    if (direction == direction_)
        return;
    direction_ = direction;
    shape_dirty_ = wrap_dirty_ = true;
}

void lx::text::text_layout::set_language(const char* bcp47) {
    char next[sizeof(language_)]{};
    for (std::size_t i = 0; bcp47 && bcp47[i] && i + 1 < sizeof(next); ++i)
        next[i] = bcp47[i];
    if (std::memcmp(next, language_, sizeof(next)) == 0)
        return;
    std::memcpy(language_, next, sizeof(next));
    shape_dirty_ = wrap_dirty_ = true;
}

void lx::text::text_layout::set_align(text_align align) {
    if (align == align_)
        return;
    align_ = align;
    wrap_dirty_ = true;
}

void lx::text::text_layout::set_wrap(text_wrap wrap) {
    if (wrap == wrap_)
        return;
    wrap_ = wrap;
    wrap_dirty_ = true;
}

void lx::text::text_layout::set_max_lines(unsigned lines) {
    if (lines == max_lines_)
        return;
    max_lines_ = lines;
    wrap_dirty_ = true;
}

void lx::text::text_layout::set_ellipsize(bool on) {
    if (on == ellipsize_)
        return;
    ellipsize_ = on;
    wrap_dirty_ = true;
}

void lx::text::text_layout::set_line_height(float multiple) {
    if (!(multiple > 0.f))
        multiple = 0.f;
    if (multiple == line_height_)
        return;
    line_height_ = multiple;
    wrap_dirty_ = true;
}

void lx::text::text_layout::set_limits(text_layout_limits limits) {
    if (limits == limits_)
        return;
    limits_ = limits;
    if (text_.size() > limits_.max_bytes) {
        std::size_t n = limits_.max_bytes;
        while (n > 0 && (static_cast<unsigned char>(text_[n]) & 0xC0u) == 0x80u)
            --n;
        text_.resize(n);
        input_cut_ = true;
    }
    if (attrs_.size() > limits_.max_attrs) {
        attrs_.resize(limits_.max_attrs);
        input_cut_ = true;
    }
    shape_dirty_ = wrap_dirty_ = true;
}

void lx::text::text_layout::set_engine(text_engine engine) {
    if (engine == engine_)
        return;
    engine_ = engine;
    shape_dirty_ = wrap_dirty_ = true;
}

// ── Passes ──

lx::result<void> lx::text::text_layout::update() { return refresh(); }

lx::result<void> lx::text::text_layout::refresh() const {
    if (shape_dirty_) {
        shape_dirty_ = false;
        wrap_dirty_ = true;
        error_ = {};
        if (const lx::result<void> r = shape_pass(); !r) {
            error_ = r.get_error();
            paragraphs_.clear();
            items_.clear();
            glyphs_.clear();
            clusters_.clear();
            lines_.clear();
            line_runs_.clear();
            width_ = height_ = 0;
            wrap_dirty_ = false;
            return error_;
        }
    }
    if (wrap_dirty_) {
        wrap_dirty_ = false;
        if (error_.domain == lx::error_domain::none)
            if (const lx::result<void> r = wrap_pass(); !r)
                error_ = r.get_error();
    }
    if (error_.domain != lx::error_domain::none)
        return error_;
    return {};
}

bool lx::text::text_layout::over_cap(bool hit) const {
    if (!hit)
        return false;
    if (limits_.overflow == lx::runtime::overflow_action::warn_and_drop && !warned_) {
        warned_ = true;
        lx::trace::logger::global().log(lx::trace::level::warn, "text",
                                        "text_layout: a storage cap cut the text");
    }
    return true;
}

std::uint8_t lx::text::text_layout::face_slot_locked(face_cache& faces, face_id id) const {
    for (std::size_t i = 0; i < faces_.size(); ++i)
        if (faces_[i].id == id)
            return static_cast<std::uint8_t>(i);
    if (faces_.size() >= k_layout_max_faces)
        return 0; // the base face's metrics stand in
    face_entry e{};
    e.id = id;
    if (const lx::result<face_metrics> m = faces.metrics_locked(id, size_26_6_); m)
        e.m = *m;
    else if (!faces_.empty())
        e.m = faces_[0].m;
    faces_.push_back(e);
    return static_cast<std::uint8_t>(faces_.size() - 1);
}

lx::result<bool> lx::text::text_layout::shape_item(face_cache& faces, shape_cache& shapes,
                                                   std::uint32_t begin, std::uint32_t end,
                                                   const text_run& run) const {
    shape_request req{};
    req.face = run.face;
    req.size_26_6 = size_26_6_;
    req.utf8 = std::string_view{text_.data() + begin, end - begin};
    req.direction = run.rtl() ? text_direction::rtl : text_direction::ltr;
    req.script = run.script;
    req.language = language_[0] ? language_ : nullptr;
    const lx::result<shaped_run> r = shapes.shape_locked(faces, req);
    if (!r)
        return r.get_error();
    ++counters_.shape_requests;
    if (!r->hit)
        ++counters_.shape_misses;
    const std::span<const shaped_glyph> g = r->glyphs;
    const auto m = static_cast<std::uint32_t>(g.size());
    if (over_cap(items_.size() >= limits_.max_runs ||
                 glyphs_.size() + m > limits_.max_glyphs)) {
        shape_cut_ = true;
        if (limits_.overflow == lx::runtime::overflow_action::reject)
            return lx::invalid_argument("text_layout: glyph or run cap reached");
        return false;
    }

    item it{};
    it.begin = begin;
    it.end = end;
    it.glyph_begin = static_cast<std::uint32_t>(glyphs_.size());
    it.glyph_count = m;
    it.face = run.face;
    it.script = run.script;
    it.attr = run.span;
    it.level = run.level;
    it.fallback = run.fallback;
    it.face_slot = face_slot_locked(faces, run.face);
    const auto index = static_cast<std::uint32_t>(items_.size());
    items_.push_back(it);
    for (const shaped_glyph& s : g)
        glyphs_.push_back({s.glyph, begin + s.cluster, s.x_advance, s.x_offset, s.y_offset});

    // Clusters in logical order: HarfBuzz's (monotone) clusters, then merged until each ends
    // on a grapheme boundary.
    const std::size_t first = clusters_.size();
    const std::uint32_t G = it.glyph_begin;
    if (m == 0) {
        clusters_.push_back({begin, end, 0, G, 0, index, 1, false});
    } else if (!run.rtl()) {
        for (std::uint32_t k = 0; k < m;) {
            const std::uint32_t c = g[k].cluster;
            std::uint32_t k2 = k;
            std::int32_t adv = 0;
            while (k2 < m && g[k2].cluster == c)
                adv += g[k2++].x_advance;
            const std::uint32_t next = k2 < m ? begin + g[k2].cluster : end;
            clusters_.push_back({begin + c, next, adv, G + k, k2 - k, index, 1, false});
            k = k2;
        }
    } else {
        for (std::int64_t k = static_cast<std::int64_t>(m) - 1; k >= 0;) {
            const std::uint32_t c = g[static_cast<std::size_t>(k)].cluster;
            std::int64_t k2 = k;
            std::int32_t adv = 0;
            while (k2 >= 0 && g[static_cast<std::size_t>(k2)].cluster == c)
                adv += g[static_cast<std::size_t>(k2--)].x_advance;
            const std::uint32_t next =
                k2 >= 0 ? begin + g[static_cast<std::size_t>(k2)].cluster : end;
            clusters_.push_back({begin + c, next, adv, G + static_cast<std::uint32_t>(k2 + 1),
                                 static_cast<std::uint32_t>(k - k2), index, 1, false});
            k = k2;
        }
    }
    clusters_[first].begin = begin;
    // Merge to grapheme boundaries (in place).
    std::size_t out = first;
    for (std::size_t i = first; i < clusters_.size(); ++i) {
        layout_cluster c = clusters_[i];
        if (out > first && !(pos_[clusters_[out - 1].end] & k_pos_grapheme)) {
            layout_cluster& prev = clusters_[out - 1];
            prev.end = c.end;
            prev.advance += c.advance;
            if (run.rtl())
                prev.glyph_begin = c.glyph_begin;
            prev.glyph_count += c.glyph_count;
            continue;
        }
        clusters_[out++] = c;
    }
    clusters_.resize(out);
    for (std::size_t i = first; i < clusters_.size(); ++i) {
        layout_cluster& c = clusters_[i];
        unsigned graphemes = 1;
        bool space = true;
        for (std::uint32_t b = c.begin; b < c.end; ++b) {
            if (b > c.begin && (pos_[b] & k_pos_grapheme))
                ++graphemes;
            if ((pos_[b] & k_pos_char) && !(pos_[b] & k_pos_space))
                space = false;
        }
        c.graphemes = static_cast<std::uint16_t>(graphemes < 0xFFFFu ? graphemes : 0xFFFFu);
        c.space = space;
    }
    return true;
}

lx::result<void> lx::text::text_layout::shape_pass() const {
    ++counters_.shape_passes;
    pos_.assign(text_.size() + 1, 0);
    paragraphs_.clear();
    items_.clear();
    glyphs_.clear();
    clusters_.clear();
    faces_.clear();
    ellipsis_glyphs_.clear();
    estimate_ = false;
    shape_cut_ = false;
    if (input_cut_) {
        if (limits_.overflow == lx::runtime::overflow_action::reject)
            return lx::invalid_argument("text_layout: text or attributes over the cap");
        shape_cut_ = over_cap(true);
    }
    const text_engine e = engine();
    face_cache& faces = *e.faces;
    auto lock = faces.lock();

    const font_desc f = font();
    size_26_6_ = size_26_6_from_pt(f.size_pt * scale_);
    base_face_ = size_26_6_ > 0 ? faces.resolve_locked(f.family, f.weight, f.style) : face_id{};
    const auto n = static_cast<std::uint32_t>(text_.size());
    const char* s = text_.data();

    // Paragraphs (and, without a face, nothing else: widths are estimated in the wrap pass).
    if (!base_face_.valid())
        estimate_ = true;
    else
        (void)face_slot_locked(faces, base_face_);
    face_id styled[2][2]{};
    const auto face_for = [&](weight w, style st) {
        face_id& id = styled[w == weight::bold][st == style::italic];
        if (!id.valid())
            id = (w == f.weight && st == f.style) ? base_face_
                                                  : faces.resolve_locked(f.family, w, st);
        return id.valid() ? id : base_face_;
    };

    std::uint32_t pb = 0;
    while (true) {
        std::uint32_t pe = pb;
        std::uint32_t sep = 0;
        while (pe < n && (sep = separator_at(s, pe, n)) == 0)
            ++pe;
        pos_[pb] |= k_pos_char | k_pos_grapheme | k_pos_break;
        pos_[pe] |= k_pos_char | k_pos_grapheme | k_pos_break;
        if (estimate_) {
            const auto level = static_cast<std::uint8_t>(direction_ == text_direction::rtl);
            paragraphs_.push_back({pb, pe, 0, 0, level});
            for (std::uint32_t b = pb; b < pe; ++b)
                if ((static_cast<unsigned char>(s[b]) & 0xC0u) != 0x80u)
                    pos_[b] |= k_pos_char | k_pos_grapheme;
        } else if (pb == pe) {
            const auto cl = static_cast<std::uint32_t>(clusters_.size());
            std::uint8_t level = direction_ == text_direction::rtl ? 1 : 0;
            paragraphs_.push_back({pb, pe, cl, cl, level});
        }
        for (std::uint32_t cb = pb; !estimate_ && cb < pe;) {
            ranges_.clear();
            for (std::size_t a = 0; a < attrs_.size(); ++a) {
                const text_attr& at = attrs_[a];
                if (at.end <= cb || at.begin >= pe)
                    continue;
                const std::uint32_t rb = at.begin > cb ? at.begin : cb;
                const std::uint32_t re = at.end < pe ? at.end : pe;
                ranges_.push_back({rb - cb, re - cb, face_for(at.weight, at.style),
                                   static_cast<std::uint16_t>(a)});
            }
            itemize_request req{};
            req.utf8 = std::string_view{s + cb, pe - cb};
            req.face = base_face_;
            req.faces = ranges_;
            req.direction = direction_;
            req.language = language_[0] ? language_ : nullptr;
            const lx::result<itemized_paragraph> ip = e.items->itemize_locked(faces, req);
            if (!ip)
                return ip.get_error();
            const itemizer& items = *e.items;
            const std::span<const std::uint32_t> off = items.char_offsets();
            const std::span<const std::uint8_t> cf = items.char_flags();
            for (std::uint32_t k = 0; k < ip->chars; ++k) {
                const std::uint32_t at = cb + off[k];
                const std::uint32_t after = cb + off[k + 1];
                pos_[at] |= k_pos_char;
                if (cf[k] & k_char_whitespace)
                    pos_[at] |= k_pos_space;
                if (cf[k] & k_char_grapheme_end)
                    pos_[after] |= k_pos_grapheme;
                if (cf[k] & k_char_break_allowed)
                    pos_[after] |= k_pos_break;
                if (cf[k] & k_char_break_mandatory)
                    pos_[after] |= k_pos_mandatory;
            }
            const std::uint32_t chunk_end = cb + ip->bytes;
            pos_[chunk_end] |= k_pos_char | k_pos_grapheme | k_pos_break;
            paragraph para{cb, chunk_end, static_cast<std::uint32_t>(clusters_.size()), 0,
                           ip->base_level};
            bool stop = false;
            for (const text_run& run : ip->runs) {
                // Pieces of at most k_layout_shape_piece_chars, cut at a break opportunity.
                for (std::uint32_t c0 = run.char_begin; c0 < run.char_end && !stop;) {
                    std::uint32_t c1 = run.char_end;
                    if (c1 - c0 > k_layout_shape_piece_chars) {
                        const std::uint32_t lim = c0 + k_layout_shape_piece_chars;
                        std::uint32_t cut = 0;
                        for (std::uint32_t k = lim; k > c0 + 1 && !cut; --k)
                            if (cf[k - 1] & k_char_break_allowed)
                                cut = k;
                        for (std::uint32_t k = lim; k > c0 + 1 && !cut; --k)
                            if (cf[k - 1] & k_char_grapheme_end)
                                cut = k;
                        c1 = cut ? cut : lim;
                    }
                    const lx::result<bool> r =
                        shape_item(faces, *e.shapes, cb + off[c0], cb + off[c1], run);
                    if (!r)
                        return r.get_error();
                    stop = !*r;
                    c0 = c1;
                }
                if (stop)
                    break;
            }
            para.cluster_end = static_cast<std::uint32_t>(clusters_.size());
            if (para.cluster_end > para.cluster_begin || !stop)
                paragraphs_.push_back(para);
            if (stop)
                return {};
            cb = chunk_end;
        }
        if (pe >= n)
            break;
        pb = pe + sep;
    }
    return {};
}

bool lx::text::text_layout::ensure_ellipsis(std::uint8_t slot) const {
    if (slot >= faces_.size())
        return false;
    face_entry& fe = faces_[slot];
    if (fe.ellipsis_ready)
        return fe.ellipsis_count > 0;
    fe.ellipsis_ready = true;
    const text_engine e = engine();
    auto lock = e.faces->lock();
    // "…", or three full stops when the face has no U+2026.
    const bool has = e.faces->has_char_locked(fe.id, k_ellipsis);
    shape_request req{};
    req.face = fe.id;
    req.size_26_6 = size_26_6_;
    req.utf8 = has ? std::string_view{"\xE2\x80\xA6"} : std::string_view{"..."};
    req.direction = text_direction::ltr;
    const lx::result<shaped_run> r = e.shapes->shape_locked(*e.faces, req);
    if (!r || r->glyphs.empty())
        return false;
    ++counters_.ellipsis_shapes;
    fe.ellipsis_begin = static_cast<std::uint32_t>(ellipsis_glyphs_.size());
    fe.ellipsis_count = static_cast<std::uint32_t>(r->glyphs.size());
    fe.ellipsis_advance = r->x_advance;
    for (const shaped_glyph& g : r->glyphs)
        ellipsis_glyphs_.push_back({g.glyph, 0, g.x_advance, g.x_offset, g.y_offset});
    return true;
}

void lx::text::text_layout::build_line(const paragraph& p, std::uint32_t c,
                                       std::uint32_t content_end, std::uint32_t hang_end,
                                       bool ellipsis, std::uint8_t ellipsis_slot,
                                       std::uint32_t line_end) const {
    layout_line line{};
    line.begin = c < p.cluster_end ? clusters_[c].begin : p.begin;
    if (c == p.cluster_begin)
        line.begin = p.begin;
    line.end = line_end;
    line.paragraph = static_cast<std::uint32_t>(paragraphs_.size() ? &p - paragraphs_.data() : 0);
    line.level = p.level;
    line.ellipsized = ellipsis;
    line.run_begin = static_cast<std::uint32_t>(line_runs_.size());

    const auto push_pieces = [&](std::uint32_t from, std::uint32_t to, bool hanging) {
        for (std::uint32_t k = from; k < to;) {
            const std::uint32_t it_index = clusters_[k].item;
            std::uint32_t m = k;
            std::int32_t w = 0;
            while (m < to && clusters_[m].item == it_index)
                w += clusters_[m++].advance;
            const item& it = items_[it_index];
            const bool rtl = (it.level & 1u) != 0;
            layout_run r{};
            r.begin = clusters_[k].begin;
            r.end = clusters_[m - 1].end;
            const layout_cluster& lo = rtl ? clusters_[m - 1] : clusters_[k];
            const layout_cluster& hi = rtl ? clusters_[k] : clusters_[m - 1];
            r.glyph_begin = lo.glyph_begin;
            r.glyph_count = hi.glyph_begin + hi.glyph_count - lo.glyph_begin;
            r.cluster_begin = k;
            r.cluster_count = m - k;
            r.width = w;
            r.face = it.face;
            r.size_26_6 = size_26_6_;
            r.script = it.script;
            r.attr = it.attr;
            r.level = hanging ? p.level : it.level;
            r.rtl = rtl;
            r.hanging = hanging;
            r.fallback = it.fallback;
            line_runs_.push_back(r);
            k = m;
        }
    };
    push_pieces(c, content_end, false);
    push_pieces(content_end, hang_end, true);
    if (ellipsis && ellipsis_slot < faces_.size() && faces_[ellipsis_slot].ellipsis_count) {
        const face_entry& fe = faces_[ellipsis_slot];
        layout_run r{};
        r.begin = r.end = line_end;
        r.glyph_begin = fe.ellipsis_begin;
        r.glyph_count = fe.ellipsis_count;
        r.cluster_begin = content_end;
        r.width = fe.ellipsis_advance;
        r.face = fe.id;
        r.size_26_6 = size_26_6_;
        r.level = p.level;
        r.ellipsis = true;
        // The cut's attribute span, so the ellipsis takes the text's colour.
        r.attr = content_end > c ? items_[clusters_[content_end - 1].item].attr : k_no_attr;
        line_runs_.push_back(r);
    }
    line.run_count = static_cast<std::uint32_t>(line_runs_.size()) - line.run_begin;

    // UAX #9 L2: from the highest level to the lowest odd level on the line, reverse every
    // maximal sequence of runs at that level or above.
    layout_run* rb = line_runs_.data() + line.run_begin;
    layout_run* re = rb + line.run_count;
    std::uint8_t hi = 0;
    std::uint8_t lo = 0xFF;
    for (const layout_run* r = rb; r != re; ++r) {
        hi = r->level > hi ? r->level : hi;
        lo = r->level < lo ? r->level : lo;
    }
    if (line.run_count) {
        const unsigned lowest_odd = lo | 1u;
        for (unsigned lev = hi; lev >= lowest_odd && lev > 0; --lev) {
            for (layout_run* r = rb; r != re;) {
                if (r->level < lev) {
                    ++r;
                    continue;
                }
                layout_run* q = r;
                while (q != re && q->level >= lev)
                    ++q;
                std::reverse(r, q);
                r = q;
            }
        }
    }

    // Positions relative to the content's left edge (alignment is added once the layout's
    // width is known); hanging runs before the content push it right.
    std::int32_t lead = 0;
    std::int32_t width = 0;
    bool content_seen = false;
    for (const layout_run* r = rb; r != re; ++r) {
        if (r->hanging) {
            if (!content_seen)
                lead += r->width;
        } else {
            content_seen = true;
            width += r->width;
        }
    }
    std::int32_t x = -lead;
    std::int32_t ascent = 0;
    std::int32_t descent = 0;
    for (layout_run* r = rb; r != re; ++r) {
        r->x = x;
        x += r->width;
        const face_metrics* m = metrics_of(r->face);
        if (m) {
            ascent = m->ascent > ascent ? m->ascent : ascent;
            descent = m->descent > descent ? m->descent : descent;
        }
    }
    const face_metrics& base = faces_.empty() ? face_metrics{} : faces_[0].m;
    if (ascent == 0 && descent == 0) {
        ascent = base.ascent;
        descent = base.descent;
    }
    line.width = width;
    line.ascent = ascent;
    line.descent = descent;
    if (line_height_ > 0.f)
        line.height = round_px(static_cast<std::int32_t>(line_height_ * size_26_6_));
    else
        line.height = round_px(ascent + descent + base.line_gap);
    lines_.push_back(line);
}

lx::result<void> lx::text::text_layout::wrap_pass() const {
    ++counters_.wrap_passes;
    lines_.clear();
    line_runs_.clear();
    lines_cut_ = false;
    ellipsized_ = false;
    const std::int32_t max = to_26_6(max_width_ * scale_);
    const bool wrap = wrap_ == text_wrap::word && max > 0;

    if (estimate_) {
        // No face: one line per paragraph, widths as `font_stack` estimates them.
        const auto est = static_cast<std::int32_t>(size_pt_ * scale_ * 0.55f * 64.f);
        const std::int32_t h = round_px(static_cast<std::int32_t>(size_pt_ * scale_ * 1.2f * 64.f));
        for (const paragraph& p : paragraphs_) {
            if (lines_.size() >= limits_.max_lines) {
                lines_cut_ = over_cap(true);
                break;
            }
            std::int32_t w = 0;
            for (std::uint32_t b = p.begin; b < p.end; ++b)
                if (pos_[b] & k_pos_char)
                    w += est;
            layout_line l{};
            l.begin = p.begin;
            l.end = p.end;
            l.paragraph = static_cast<std::uint32_t>(&p - paragraphs_.data());
            l.width = w;
            l.height = h;
            l.ascent = h * 4 / 5;
            l.descent = h - l.ascent;
            l.level = p.level;
            l.paragraph_end = true;
            lines_.push_back(l);
        }
    } else {
        bool stop = false;
        for (std::size_t pi = 0; pi < paragraphs_.size() && !stop; ++pi) {
            const paragraph& p = paragraphs_[pi];
            const bool last_paragraph = pi + 1 == paragraphs_.size();
            std::uint32_t c = p.cluster_begin;
            const std::uint32_t ce = p.cluster_end;
            do {
                if (lines_.size() >= limits_.max_lines) {
                    lines_cut_ = over_cap(true);
                    stop = true;
                    break;
                }
                const bool last_allowed = max_lines_ > 0 && lines_.size() + 1 == max_lines_;
                // Greedy: the longest run of clusters from c whose content (trailing
                // whitespace hangs) fits, ending at a break opportunity when one exists.
                std::int32_t width = 0;
                std::int32_t content = 0;
                std::uint32_t best = k_npos;
                std::uint32_t k = c;
                bool overflow = false;
                while (k < ce) {
                    const layout_cluster& cl = clusters_[k];
                    const std::int32_t nw = width + cl.advance;
                    const std::int32_t nc = cl.space ? content : nw;
                    if (wrap && nc > max && k > c) {
                        overflow = true;
                        break;
                    }
                    width = nw;
                    content = nc;
                    ++k;
                    if (k < ce) {
                        const std::uint8_t f = pos_[cl.end];
                        if (f & k_pos_mandatory)
                            break;
                        if (f & k_pos_break)
                            best = k;
                    }
                }
                std::uint32_t end = k;
                if (overflow && best != k_npos && best > c)
                    end = best;
                std::uint32_t content_end = end;
                while (content_end > c && clusters_[content_end - 1].space)
                    --content_end;
                std::int32_t content_w = 0;
                for (std::uint32_t i = c; i < content_end; ++i)
                    content_w += clusters_[i].advance;

                const bool more = end < ce || !last_paragraph;
                bool ell = ellipsize_ && ((last_allowed && more) ||
                                          (wrap_ == text_wrap::none && max > 0 && content_w > max));
                std::uint8_t slot = 0;
                std::uint32_t line_end = end < ce ? clusters_[end].begin : p.end;
                if (ell) {
                    const std::uint32_t ref =
                        content_end > c ? content_end - 1 : (c < ce ? c : k_npos);
                    slot = ref != k_npos ? items_[clusters_[ref].item].face_slot : 0;
                    if (!ensure_ellipsis(slot))
                        slot = 0;
                    if (!ensure_ellipsis(slot)) {
                        ell = false; // no ellipsis glyphs at all: plain cut
                    }
                }
                if (ell) {
                    const std::int32_t ew = faces_[slot].ellipsis_advance;
                    std::uint32_t cut = c;
                    if (max <= 0) {
                        cut = content_end;
                    } else if (ew <= max) {
                        std::int32_t w = 0;
                        for (std::uint32_t i = c; i < end; ++i) {
                            w += clusters_[i].advance;
                            if (w + ew > max)
                                break;
                            if (!clusters_[i].space)
                                cut = i + 1;
                        }
                    }
                    const std::uint32_t cb = cut > c ? clusters_[cut - 1].end
                                                     : (c < ce ? clusters_[c].begin : p.begin);
                    if (max > 0 && ew > max) {
                        // Not even the ellipsis fits: the line is empty.
                        build_line(p, c, c, c, false, 0, cb);
                    } else {
                        build_line(p, c, cut, cut, true, slot, cb);
                        ellipsized_ = true;
                    }
                } else {
                    build_line(p, c, content_end, end, false, 0, line_end);
                }
                lines_.back().paragraph_end = end >= ce;
                if (last_allowed && more) {
                    stop = true;
                    lines_.back().paragraph_end = true;
                    break;
                }
                // An unwrapped line ends at a mandatory break or the paragraph's end, so what an
                // ellipsis hid is skipped with it.
                c = end;
            } while (c < ce);
        }
    }

    // Width, alignment, vertical stacking.
    width_ = 0;
    for (const layout_line& l : lines_)
        width_ = l.width > width_ ? l.width : width_;
    const std::int32_t box = max > 0 ? max : width_;
    std::int32_t top = 0;
    for (layout_line& l : lines_) {
        std::int32_t x = 0;
        const bool rtl = l.rtl();
        switch (align_) {
        case text_align::start: x = rtl ? box - l.width : 0; break;
        case text_align::end: x = rtl ? 0 : box - l.width; break;
        case text_align::center: x = (box - l.width) / 2; break;
        }
        if (x < 0)
            x = 0;
        l.x = x;
        for (std::uint32_t i = 0; i < l.run_count; ++i)
            line_runs_[l.run_begin + i].x += x;
        l.top = top;
        const std::int32_t leading = l.height - (l.ascent + l.descent);
        l.baseline = top + ((leading / 2) & ~63) + l.ascent;
        top += l.height;
    }
    height_ = top;
    return {};
}

// ── Queries ──

const lx::text::face_metrics* lx::text::text_layout::metrics_of(face_id face) const {
    for (const face_entry& e : faces_)
        if (e.id == face)
            return &e.m;
    return nullptr;
}

const lx::text::face_metrics* lx::text::text_layout::metrics(face_id face) const {
    (void)refresh();
    return metrics_of(face);
}

lx::size2i lx::text::text_layout::bounding_size() const {
    (void)refresh();
    int w = (width_ + 63) / 64;
    const int h = (height_ + 63) / 64;
    const std::int32_t max = to_26_6(max_width_ * scale_);
    if (max > 0 && w > (max + 63) / 64)
        w = (max + 63) / 64;
    return {w, h};
}

std::span<const lx::text::layout_line> lx::text::text_layout::lines() const {
    (void)refresh();
    return lines_;
}

std::span<const lx::text::layout_run> lx::text::text_layout::runs() const {
    (void)refresh();
    return line_runs_;
}

std::span<const lx::text::layout_run> lx::text::text_layout::runs(const layout_line& line) const {
    (void)refresh();
    if (line.run_begin + line.run_count > line_runs_.size())
        return {};
    return {line_runs_.data() + line.run_begin, line.run_count};
}

std::span<const lx::text::layout_glyph> lx::text::text_layout::glyphs(const layout_run& run) const {
    (void)refresh();
    const std::vector<layout_glyph>& pool = run.ellipsis ? ellipsis_glyphs_ : glyphs_;
    if (std::size_t{run.glyph_begin} + run.glyph_count > pool.size())
        return {};
    return {pool.data() + run.glyph_begin, run.glyph_count};
}

std::span<const lx::text::layout_cluster> lx::text::text_layout::clusters() const {
    (void)refresh();
    return clusters_;
}

lx::text::face_id lx::text::text_layout::base_face() const {
    (void)refresh();
    return base_face_;
}

std::int32_t lx::text::text_layout::size_26_6() const {
    (void)refresh();
    return size_26_6_;
}

std::int32_t lx::text::text_layout::width() const {
    (void)refresh();
    return width_;
}

std::int32_t lx::text::text_layout::height() const {
    (void)refresh();
    return height_;
}

std::int32_t lx::text::text_layout::first_baseline() const {
    (void)refresh();
    return lines_.empty() ? 0 : lines_.front().baseline;
}

lx::text::text_direction lx::text::text_layout::direction() const {
    (void)refresh();
    if (paragraphs_.empty())
        return direction_ == text_direction::rtl ? text_direction::rtl : text_direction::ltr;
    return paragraphs_.front().level & 1u ? text_direction::rtl : text_direction::ltr;
}

bool lx::text::text_layout::truncated() const {
    (void)refresh();
    return shape_cut_ || lines_cut_;
}

bool lx::text::text_layout::ellipsized() const {
    (void)refresh();
    return ellipsized_;
}

bool lx::text::text_layout::has_faces() const {
    (void)refresh();
    return !estimate_ && base_face_.valid();
}

std::size_t lx::text::text_layout::storage_bytes() const {
    return text_.capacity() + attrs_.capacity() * sizeof(text_attr) + pos_.capacity() +
           paragraphs_.capacity() * sizeof(paragraph) + items_.capacity() * sizeof(item) +
           glyphs_.capacity() * sizeof(layout_glyph) +
           clusters_.capacity() * sizeof(layout_cluster) +
           ranges_.capacity() * sizeof(face_range) + faces_.capacity() * sizeof(face_entry) +
           ellipsis_glyphs_.capacity() * sizeof(layout_glyph) +
           lines_.capacity() * sizeof(layout_line) + line_runs_.capacity() * sizeof(layout_run);
}

// ── Carets and hit testing ──

bool lx::text::text_layout::is_caret_stop(std::uint32_t index) const {
    (void)refresh();
    return index < pos_.size() && (pos_[index] & k_pos_grapheme);
}

bool lx::text::text_layout::break_opportunity(std::uint32_t index) const {
    (void)refresh();
    return index < pos_.size() && (pos_[index] & k_pos_break);
}

std::uint32_t lx::text::text_layout::next_caret(std::uint32_t index) const {
    (void)refresh();
    const auto n = static_cast<std::uint32_t>(text_.size());
    if (index >= n)
        return n;
    for (std::uint32_t p = index + 1; p < n; ++p)
        if (pos_[p] & k_pos_grapheme)
            return p;
    return n;
}

std::uint32_t lx::text::text_layout::prev_caret(std::uint32_t index) const {
    (void)refresh();
    const auto n = static_cast<std::uint32_t>(text_.size());
    if (index > n)
        index = n;
    while (index > 0) {
        --index;
        if (pos_[index] & k_pos_grapheme)
            return index;
    }
    return 0;
}

std::uint32_t lx::text::text_layout::grapheme_boundary(const layout_cluster& c, unsigned n) const {
    if (n == 0)
        return c.begin;
    unsigned seen = 0;
    for (std::uint32_t b = c.begin + 1; b < c.end; ++b)
        if ((pos_[b] & k_pos_grapheme) && ++seen == n)
            return b;
    return c.end;
}

unsigned lx::text::text_layout::graphemes_before(const layout_cluster& c,
                                                std::uint32_t index) const {
    if (index >= c.end)
        return c.graphemes;
    unsigned n = 0;
    for (std::uint32_t b = c.begin + 1; b <= index && b < c.end; ++b)
        if (pos_[b] & k_pos_grapheme)
            ++n;
    return n;
}

std::uint32_t lx::text::text_layout::line_of(std::uint32_t index, bool upstream) const {
    // The last line starting at or before the index…
    std::uint32_t lo = 0;
    std::uint32_t hi = static_cast<std::uint32_t>(lines_.size());
    while (hi - lo > 1) {
        const std::uint32_t mid = (lo + hi) / 2;
        if (lines_[mid].begin <= index)
            lo = mid;
        else
            hi = mid;
    }
    // …or, upstream at a soft break, the line that ends there.
    if (upstream && lo > 0 && lines_[lo].begin == index && lines_[lo - 1].end == index &&
        lines_[lo - 1].paragraph == lines_[lo].paragraph)
        --lo;
    return lo;
}

lx::text::caret_pos lx::text::text_layout::caret(std::uint32_t index, bool upstream) const {
    (void)refresh();
    caret_pos out{};
    if (lines_.empty())
        return out;
    const auto n = static_cast<std::uint32_t>(text_.size());
    if (index > n)
        index = n;
    out.line = line_of(index, upstream);
    const layout_line& l = lines_[out.line];
    out.top = l.top;
    out.height = l.height;
    out.rtl = l.rtl();
    // The start edge of an empty line (and of a line the index is not on).
    out.x = l.rtl() ? l.x + l.width : l.x;
    if (index > l.end)
        index = l.end;
    for (int pass = 0; pass < 2; ++pass) {
        const bool up = pass == 0 ? upstream : !upstream;
        for (std::uint32_t i = 0; i < l.run_count; ++i) {
            const layout_run& r = line_runs_[l.run_begin + i];
            if (r.ellipsis)
                continue;
            // Clusters in visual order from r.x.
            std::int32_t x = r.x;
            for (std::uint32_t v = 0; v < r.cluster_count; ++v) {
                const std::uint32_t ci =
                    r.rtl ? r.cluster_begin + r.cluster_count - 1 - v : r.cluster_begin + v;
                const layout_cluster& c = clusters_[ci];
                const bool hit = up ? (c.begin < index && index <= c.end)
                                    : (c.begin <= index && index < c.end);
                if (hit) {
                    const std::int32_t off = static_cast<std::int32_t>(
                        static_cast<std::int64_t>(c.advance) * graphemes_before(c, index) /
                        (c.graphemes ? c.graphemes : 1));
                    out.x = r.rtl ? x + c.advance - off : x + off;
                    out.rtl = r.rtl;
                    return out;
                }
                x += c.advance;
            }
        }
    }
    return out;
}

lx::text::text_hit lx::text::text_layout::hit_test(std::int32_t x, std::int32_t y) const {
    (void)refresh();
    text_hit out{};
    if (lines_.empty())
        return out;
    std::uint32_t li = 0;
    while (li + 1 < lines_.size() && y >= lines_[li].top + lines_[li].height)
        ++li;
    const layout_line& l = lines_[li];
    out.line = li;
    out.index = l.begin;
    out.inside = y >= 0 && y < height_ && x >= l.x && x < l.x + l.width;
    // The visual cluster under x (the first or last when x is beyond the line's runs).
    const layout_run* hit_run = nullptr;
    const layout_cluster* hit_cluster = nullptr;
    std::int32_t hit_left = 0;
    const layout_run* last_run = nullptr;
    const layout_cluster* last_cluster = nullptr;
    std::int32_t last_left = 0;
    bool before_first = false;
    for (std::uint32_t i = 0; i < l.run_count && !hit_run; ++i) {
        const layout_run& r = line_runs_[l.run_begin + i];
        if (r.ellipsis) {
            if (x >= r.x && x < r.x + r.width) {
                out.index = l.end;
                out.upstream = true;
                return out;
            }
            continue;
        }
        std::int32_t cx = r.x;
        for (std::uint32_t v = 0; v < r.cluster_count; ++v) {
            const std::uint32_t ci =
                r.rtl ? r.cluster_begin + r.cluster_count - 1 - v : r.cluster_begin + v;
            const layout_cluster& c = clusters_[ci];
            if (!last_cluster && x < cx)
                before_first = true;
            if (before_first || (x >= cx && x < cx + c.advance)) {
                hit_run = &r;
                hit_cluster = &c;
                hit_left = cx;
                break;
            }
            last_run = &r;
            last_cluster = &c;
            last_left = cx;
            cx += c.advance;
        }
    }
    bool right_end = false;
    if (!hit_cluster) {
        if (!last_cluster) {
            // An empty line, or only an ellipsis left of x.
            if (l.ellipsized) {
                out.index = l.end;
                out.upstream = true;
            }
            return out;
        }
        hit_run = last_run;
        hit_cluster = last_cluster;
        hit_left = last_left;
        right_end = true;
    }
    const layout_cluster& c = *hit_cluster;
    const unsigned g = c.graphemes ? c.graphemes : 1;
    // The nearest of the cluster's g + 1 grapheme edges, counted from its left.
    unsigned edge = 0;
    if (right_end) {
        edge = g;
    } else if (!before_first && c.advance > 0) {
        const std::int64_t dx = static_cast<std::int64_t>(x - hit_left) * g;
        const auto slice = static_cast<unsigned>(dx / c.advance);
        const std::int64_t in = dx - static_cast<std::int64_t>(slice) * c.advance;
        edge = slice + (in * 2 >= c.advance ? 1u : 0u);
        if (edge > g)
            edge = g;
    }
    // Logical boundary: from the left in LTR glyph order, from the right in RTL.
    const unsigned logical = hit_run->rtl ? g - edge : edge;
    out.index = grapheme_boundary(c, logical);
    out.upstream = logical == g;
    return out;
}
