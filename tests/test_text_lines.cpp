// lx.text:itemize and lx.text:lines — bidi itemization, line breaking, ellipsis,
// alignment, hit testing and the retained text_layout (ADR-010 §4.6, Phase 2).
//
// Expected values come from the Unicode algorithms, derived in the comments: UAX #9 (bidi)
// for levels and visual order, UAX #14 for break opportunities, UAX #29 for grapheme
// clusters. Widths come from the fonts, so tests that need a width measure it (with
// `shape_line`, the single-line shaper of stage 1) rather than assume one. Tests that need
// fribidi or libunibreak print SKIP when the build has neither.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.text;

namespace lt = lx::text;

namespace {

bool skip_without(bool bidi, bool segmentation, const char* what) {
    if (bidi && !lt::bidi_available()) {
        std::printf("    SKIP: %s needs fribidi (built without LUMEN_HAS_FRIBIDI)\n", what);
        return true;
    }
    if (segmentation && !lt::segmentation_available()) {
        std::printf("    SKIP: %s needs libunibreak (built without LUMEN_HAS_UNIBREAK)\n", what);
        return true;
    }
    return false;
}

/// A layout over the shared engine with the UI font at 11 pt (or SKIP when no face).
bool make(lt::text_layout& l, std::string_view text, const char* what) {
    l.set_text(text);
    l.set_font({});
    if (!l.update() || !l.has_faces()) {
        std::printf("    SKIP: %s: no font face on this host\n", what);
        return false;
    }
    return true;
}

/// The cluster of each glyph of a line, left to right, consecutive repeats collapsed.
std::vector<std::uint32_t> visual_clusters(const lt::text_layout& l, const lt::layout_line& line) {
    std::vector<std::uint32_t> out;
    for (const lt::layout_run& r : l.runs(line)) {
        if (r.ellipsis)
            continue;
        for (const lt::layout_glyph& g : l.glyphs(r))
            if (out.empty() || out.back() != g.cluster)
                out.push_back(g.cluster);
    }
    return out;
}

struct level_run {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    unsigned level = 0;
    friend bool operator==(const level_run&, const level_run&) = default;
};

/// A line's runs in visual order, adjacent runs of one level with touching byte ranges
/// merged (so a face change inside a level run does not show).
std::vector<level_run> level_runs(const lt::text_layout& l, const lt::layout_line& line) {
    std::vector<level_run> out;
    for (const lt::layout_run& r : l.runs(line)) {
        if (r.ellipsis)
            continue;
        if (!out.empty() && out.back().level == r.level &&
            (out.back().end == r.begin || out.back().begin == r.end)) {
            out.back().begin = std::min(out.back().begin, r.begin);
            out.back().end = std::max(out.back().end, r.end);
            continue;
        }
        out.push_back({r.begin, r.end, r.level});
    }
    return out;
}

void print_clusters(const char* name, const std::vector<std::uint32_t>& v) {
    std::printf("    %s:", name);
    for (std::uint32_t c : v)
        std::printf(" %u", c);
    std::printf("\n");
}

void print_runs(const char* name, const std::vector<level_run>& v) {
    std::printf("    %s:", name);
    for (const level_run& r : v)
        std::printf(" [%u,%u)L%u", r.begin, r.end, r.level);
    std::printf("\n");
}

/// Advances of `text` shaped whole (stage 1's single-line path), summed over the clusters
/// that start before `byte`, in pixels: the oracle for widths.
double width_before(const lt::shaped_line& s, std::uint32_t byte) {
    std::int64_t w = 0;
    for (unsigned i = 0; i < s.count; ++i)
        if (s.cluster[i] < byte)
            w += s.x_adv[i];
    return static_cast<double>(w) / 64.0;
}

constexpr std::int32_t px(double v) { return static_cast<std::int32_t>(v * 64.0); }

} // namespace

// ── Bidi: levels and visual order (UAX #9) ─────────────────────────────────────────

LUMEN_TEST(hebrew_paragraph_resolves_rtl_and_reverses) {
    if (skip_without(true, false, "hebrew_paragraph_resolves_rtl_and_reverses"))
        return;
    lt::text_layout l{};
    // שלום: ש U+05E9, ל U+05DC, ו U+05D5, ם U+05DD — two bytes each, at 0, 2, 4, 6.
    if (!make(l, "שלום", "hebrew"))
        return;
    // P2/P3: the first strong character is R → paragraph level 1. Every letter is R at
    // level 1 (I1 leaves R on an odd level). L2 reverses the level-1 sequence, so the
    // glyphs read right to left: the last letter leftmost.
    LUMEN_CHECK(l.direction() == lt::text_direction::rtl);
    LUMEN_CHECK(l.lines().size() == 1);
    const lt::layout_line& line = l.lines()[0];
    LUMEN_CHECK(line.rtl());
    const auto v = visual_clusters(l, line);
    print_clusters("visual clusters", v);
    LUMEN_CHECK((v == std::vector<std::uint32_t>{6, 4, 2, 0}));
    LUMEN_CHECK((level_runs(l, line) == std::vector<level_run>{{0, 8, 1}}));
    // The UI face (IBM Plex Sans) has no Hebrew: the run takes a fallback face that has it.
    const lt::layout_run& r = l.runs(line)[0];
    auto& faces = lt::face_cache::shared();
    auto lock = faces.lock();
    LUMEN_CHECK(faces.has_char_locked(r.face, U'ש'));
    if (!faces.has_char_locked(l.base_face(), U'ש'))
        LUMEN_CHECK(r.fallback && r.face != l.base_face());
}

LUMEN_TEST(ltr_paragraph_with_an_embedded_hebrew_word) {
    if (skip_without(true, false, "ltr_paragraph_with_an_embedded_hebrew_word"))
        return;
    lt::text_layout l{};
    // "abc אבג def": a0 b1 c2 sp3 א4 ב6 ג8 sp10 d11 e12 f13.
    if (!make(l, "abc אבג def", "mixed ltr"))
        return;
    // P2/P3: first strong is L → level 0. Letters: L at 0, R at 1 (I1). The spaces are
    // neutrals between L and R: N1 does not apply (different directions), N2 gives them the
    // embedding direction → level 0. Level runs [0,4) 0, [4,10) 1, [10,14) 0. L2 reverses
    // the level-1 run only: abc␠ גבא ␠def.
    LUMEN_CHECK(l.direction() == lt::text_direction::ltr);
    const lt::layout_line& line = l.lines()[0];
    const auto runs = level_runs(l, line);
    print_runs("level runs", runs);
    LUMEN_CHECK((runs == std::vector<level_run>{{0, 4, 0}, {4, 10, 1}, {10, 14, 0}}));
    const auto v = visual_clusters(l, line);
    print_clusters("visual clusters", v);
    LUMEN_CHECK((v == std::vector<std::uint32_t>{0, 1, 2, 3, 8, 6, 4, 10, 11, 12, 13}));
}

LUMEN_TEST(arabic_paragraph_with_european_digits) {
    if (skip_without(true, false, "arabic_paragraph_with_european_digits"))
        return;
    lt::text_layout l{};
    // "العدد 123 هنا": ا0 ل2 ع4 د6 د8 sp10 1₁₁ 2₁₂ 3₁₃ sp14 ه15 ن17 ا19 (21 bytes).
    if (!make(l, "العدد 123 هنا", "arabic"))
        return;
    // P2/P3: first strong is AL → level 1. W3: AL → R. W2: EN whose last strong is AL →
    // AN. I2: AN on an odd level → level 2. N1: the spaces sit between R and AN (numbers
    // count as R for N1) → R → level 1. Level runs [0,11) 1, [11,14) 2, [14,21) 1.
    // L2: reverse level ≥ 2 ("123" → "321"), then level ≥ 1 (the whole line): the digits
    // read left to right again, the runs are in reverse order, each Arabic run reversed.
    LUMEN_CHECK(l.direction() == lt::text_direction::rtl);
    const lt::layout_line& line = l.lines()[0];
    const auto runs = level_runs(l, line);
    print_runs("level runs", runs);
    LUMEN_CHECK((runs == std::vector<level_run>{{14, 21, 1}, {11, 14, 2}, {0, 11, 1}}));
    const auto v = visual_clusters(l, line);
    print_clusters("visual clusters", v);
    LUMEN_CHECK((v == std::vector<std::uint32_t>{19, 17, 15, 14, 11, 12, 13, 10, 8, 6, 4, 2, 0}));
}

LUMEN_TEST(rtl_paragraph_with_an_embedded_latin_word) {
    if (skip_without(true, false, "rtl_paragraph_with_an_embedded_latin_word"))
        return;
    lt::text_layout l{};
    // "אבג abc דהו": א0 ב2 ג4 sp6 a7 b8 c9 sp10 ד11 ה13 ו15 (17 bytes).
    if (!make(l, "אבג abc דהו", "mixed rtl"))
        return;
    // Level 1 (first strong R). I2: L on an odd level → 2. The spaces sit between R and L:
    // N2 → embedding level 1. Level runs [0,7) 1, [7,10) 2, [10,17) 1. L2: "abc" is
    // reversed twice (stays LTR), the three runs are in reverse order.
    LUMEN_CHECK(l.direction() == lt::text_direction::rtl);
    const lt::layout_line& line = l.lines()[0];
    const auto runs = level_runs(l, line);
    print_runs("level runs", runs);
    LUMEN_CHECK((runs == std::vector<level_run>{{10, 17, 1}, {7, 10, 2}, {0, 7, 1}}));
    const auto v = visual_clusters(l, line);
    print_clusters("visual clusters", v);
    LUMEN_CHECK((v == std::vector<std::uint32_t>{15, 13, 11, 10, 7, 8, 9, 6, 4, 2, 0}));
}

LUMEN_TEST(forced_direction_overrides_the_first_strong_character) {
    if (skip_without(true, false, "forced_direction_overrides_the_first_strong_character"))
        return;
    lt::text_layout l{};
    if (!make(l, "abc", "forced rtl"))
        return;
    // A forced RTL paragraph is level 1; L characters on it go to level 2 (I2); the line is
    // right-aligned at `start`.
    l.set_direction(lt::text_direction::rtl);
    l.set_max_width(200.f);
    LUMEN_CHECK(l.direction() == lt::text_direction::rtl);
    const lt::layout_line& line = l.lines()[0];
    LUMEN_CHECK(line.rtl());
    LUMEN_CHECK(l.runs(line)[0].level == 2);
    LUMEN_CHECK(line.x + line.width == px(200));
}

LUMEN_TEST(itemizer_caches_fallback_faces_per_block) {
    if (skip_without(false, false, "itemizer_caches_fallback_faces_per_block"))
        return;
    auto& faces = lt::face_cache::shared();
    lt::itemizer items{};
    const lt::face_id base = faces.resolve(lt::font_desc{});
    if (!base.valid()) {
        std::printf("    SKIP: no font face on this host\n");
        return;
    }
    {
        auto lock = faces.lock();
        if (faces.has_char_locked(base, U'א')) {
            std::printf("    SKIP: the UI face has Hebrew; no fallback to exercise\n");
            return;
        }
    }
    lt::itemize_request req{};
    req.face = base;
    req.utf8 = "אבג דהו זחט";
    const auto first = items.itemize(faces, req);
    LUMEN_CHECK(first.ok());
    const lt::itemizer_counters c1 = items.counters();
    std::printf("    first: %zu runs, %llu searches, %llu table hits\n", first->runs.size(),
                static_cast<unsigned long long>(c1.fallback_searches),
                static_cast<unsigned long long>(c1.fallback_hits));
    // One search for the block (U+0580–U+05FF), table hits for every later letter; the
    // spaces stay in the run's face, so the paragraph is one run.
    LUMEN_CHECK(c1.fallback_searches == 1);
    LUMEN_CHECK(first->runs.size() == 1);
    LUMEN_CHECK(first->runs[0].fallback);
    (void)items.itemize(faces, req);
    LUMEN_CHECK(items.counters().fallback_searches == 1);
}

// ── Line breaking (UAX #14) and wrapping ───────────────────────────────────────────

LUMEN_TEST(latin_and_cyrillic_wrap_at_spaces_for_several_widths) {
    if (skip_without(false, true, "latin_and_cyrillic_wrap_at_spaces_for_several_widths"))
        return;
    const std::string_view text =
        "Lorem ipsum dolor sit amet, съешь же ещё этих мягких французских булок";
    lt::shaped_line whole{};
    if (!lt::shape_line(std::string{text}.c_str(), lt::font_desc{}, whole)) {
        std::printf("    SKIP: no font face on this host\n");
        return;
    }
    // Word boundaries: UAX #14 LB18 allows a break after spaces only (letters are AL, the
    // comma IS — LB13/LB29 keep it with the word before), so the candidates are the starts
    // of words. For a first line ending after word k, its content (without the trailing
    // space, which hangs) must fit and word k + 1 must not: width between the two.
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> ends{};
    for (std::uint32_t i = 0; i < text.size(); ++i)
        if (text[i] == ' ') {
            ends.push_back(i);
            starts.push_back(i + 1);
        }
    ends.push_back(static_cast<std::uint32_t>(text.size()));
    for (const unsigned k : {1u, 3u, 5u, 7u}) {
        const double fit = width_before(whole, ends[k]);
        const double next = width_before(whole, ends[k + 1]);
        const double w = (fit + next) / 2.0;
        lt::text_layout l{};
        l.set_text(text);
        l.set_max_width(static_cast<float>(w));
        const auto lines = l.lines();
        std::printf("    width %.1f px: %zu lines, first [%u,%u) %.1f px "
                    "(expected end %u, %.1f px)\n",
                    w, lines.size(), lines[0].begin, lines[0].end, lines[0].width / 64.0,
                    starts[k + 1], fit);
        LUMEN_CHECK(lines[0].begin == 0);
        LUMEN_CHECK(lines[0].end == starts[k + 1]);
        LUMEN_CHECK(std::abs(lines[0].width / 64.0 - fit) <= 1.0);
        // Every line starts where the previous ended, at a word start, and fits.
        std::uint32_t at = 0;
        for (const lt::layout_line& line : lines) {
            LUMEN_CHECK(line.begin == at);
            LUMEN_CHECK(std::find(starts.begin(), starts.end(), line.begin) != starts.end());
            LUMEN_CHECK(line.width <= px(w) + 1);
            at = line.end;
        }
        LUMEN_CHECK(at == text.size());
    }
    // Wide enough: one line.
    lt::text_layout l{};
    l.set_text(text);
    l.set_max_width(static_cast<float>(width_before(whole, ~0u) + 2.0));
    LUMEN_CHECK(l.lines().size() == 1);
}

LUMEN_TEST(min_content_width_is_the_widest_word) {
    // CSS min-content: the narrowest the text wraps without breaking a word — the widest
    // word, its trailing space hanging (what a flex container asks for an item's automatic
    // minimum size, lx.layout:flex). The words' widths come from the shaped line.
    if (skip_without(false, true, "min_content_width_is_the_widest_word"))
        return;
    const std::string_view text = "ab cdefghij kl mnopq";
    lt::shaped_line whole{};
    if (!lt::shape_line(std::string{text}.c_str(), lt::font_desc{}, whole)) {
        std::printf("    SKIP: no font face on this host\n");
        return;
    }
    // "cdefghij" spans bytes [3, 11); the widest of the four words.
    const double widest = width_before(whole, 11) - width_before(whole, 3);
    LUMEN_CHECK(widest > width_before(whole, 2) &&
                widest > width_before(whole, 20) - width_before(whole, 15));
    lt::text_layout l{};
    l.set_text(text);
    l.set_wrap(lt::text_wrap::word);
    const int min_content = l.min_content_width();
    std::printf("    min-content %d px, widest word %.2f px\n", min_content, widest);
    LUMEN_CHECK(min_content == static_cast<int>(std::ceil(widest)));
    // Wrapped there, no word is broken: four lines, one word each.
    l.set_max_width(static_cast<float>(min_content));
    LUMEN_CHECK(l.lines().size() == 4);
    LUMEN_CHECK(l.bounding_size().width == min_content);
    // A mandatory break separates words as a break opportunity does.
    lt::text_layout two{};
    two.set_text("cdefghij\nab");
    LUMEN_CHECK(two.min_content_width() == min_content);
}

LUMEN_TEST(cjk_breaks_between_ideographs) {
    if (skip_without(false, true, "cjk_breaks_between_ideographs"))
        return;
    lt::text_layout l{};
    // 一二三四五六: U+4E00 U+4E8C U+4E09 U+56DB U+4E94 U+516D, three bytes each.
    const std::string_view text = "一二三四五六";
    if (!make(l, text, "cjk"))
        return;
    // UAX #14: ideographs are class ID; no rule before LB31 forbids ID ÷ ID, so there is an
    // opportunity between every two of them (and none inside a character).
    for (std::uint32_t b = 3; b < 18; b += 3)
        LUMEN_CHECK(l.break_opportunity(b));
    LUMEN_CHECK(!l.break_opportunity(1) && !l.break_opportunity(4));
    // Fallback: a scalable face with U+4E00 if the host has one; else the primary (.notdef).
    auto& faces = lt::face_cache::shared();
    lt::face_id cjk{};
    {
        auto lock = faces.lock();
        cjk = faces.fallback_locked(l.base_face(), U'一');
    }
    const lt::layout_run& r0 = l.runs()[0];
    if (cjk.valid()) {
        LUMEN_CHECK(r0.face == cjk);
    } else {
        std::printf("    SKIP (fallback part): no scalable face with U+4E00 on this host; the "
                    "ideographs stay in the UI face as .notdef\n");
        LUMEN_CHECK(r0.face == l.base_face());
    }
    const std::int32_t adv = l.clusters()[0].advance;
    LUMEN_CHECK(adv > 0);
    // Two and a half ideographs wide: two per line (each ideograph has one advance here).
    l.set_max_width(static_cast<float>(adv * 5 / 2) / 64.f);
    const auto lines = l.lines();
    LUMEN_CHECK(lines.size() == 3);
    for (unsigned i = 0; i < lines.size() && i < 3; ++i) {
        LUMEN_CHECK(lines[i].begin == i * 6);
        LUMEN_CHECK(lines[i].end == i * 6 + 6);
    }
}

LUMEN_TEST(explicit_newlines_make_paragraphs) {
    lt::text_layout l{};
    if (!make(l, "one\ntwo\r\n\nfour", "newlines"))
        return;
    // LF, CR LF and LF: four paragraphs, the third empty; no line holds a separator.
    const auto lines = l.lines();
    LUMEN_CHECK(lines.size() == 4);
    LUMEN_CHECK(lines[0].begin == 0 && lines[0].end == 3);
    LUMEN_CHECK(lines[1].begin == 4 && lines[1].end == 7);
    LUMEN_CHECK(lines[2].begin == 9 && lines[2].end == 9 && lines[2].run_count == 0);
    LUMEN_CHECK(lines[3].begin == 10 && lines[3].end == 14);
    for (unsigned i = 1; i < lines.size(); ++i)
        LUMEN_CHECK(lines[i].top == lines[i - 1].top + lines[i - 1].height);
    LUMEN_CHECK(l.height() == lines[3].top + lines[3].height);
    // Between CR and LF there is no caret stop.
    LUMEN_CHECK(!l.is_caret_stop(8));
    LUMEN_CHECK(l.next_caret(7) == 9);
}

// ── Ellipsis ───────────────────────────────────────────────────────────────────────

LUMEN_TEST(ellipsis_at_the_logical_end_ltr) {
    lt::text_layout l{};
    const std::string_view text = "The quick brown fox jumps over the lazy dog";
    if (!make(l, text, "ellipsis ltr"))
        return;
    l.set_max_lines(1);
    l.set_ellipsize(true);
    l.set_max_width(100.f);
    LUMEN_CHECK(l.lines().size() == 1);
    LUMEN_CHECK(l.ellipsized());
    const lt::layout_line& line = l.lines()[0];
    LUMEN_CHECK(line.ellipsized && line.begin == 0 && line.end < text.size());
    LUMEN_CHECK(l.is_caret_stop(line.end));
    LUMEN_CHECK(line.width <= px(100));
    // LTR: the logical end is the visual right; the ellipsis is the last run.
    const auto runs = l.runs(line);
    const lt::layout_run& e = runs[runs.size() - 1];
    LUMEN_CHECK(e.ellipsis);
    LUMEN_CHECK(e.x + e.width == line.x + line.width);
    // The cut is the longest prefix that fits with the ellipsis: one more cluster would not.
    const auto clusters = l.clusters();
    std::int32_t content = 0;
    std::uint32_t next = 0;
    for (std::uint32_t i = 0; i < clusters.size(); ++i) {
        if (clusters[i].end <= line.end)
            content += clusters[i].advance;
        else if (!next)
            next = i;
    }
    std::printf("    cut at byte %u, %d/64 px content + %d/64 px ellipsis\n", line.end, content,
                e.width);
    LUMEN_CHECK(content + e.width <= px(100));
    if (clusters[next].begin == line.end && !clusters[next].space)
        LUMEN_CHECK(content + clusters[next].advance + e.width > px(100));
    // Two lines: the first wraps normally, the second ends in the ellipsis.
    l.set_max_lines(2);
    LUMEN_CHECK(l.lines().size() == 2);
    LUMEN_CHECK(!l.lines()[0].ellipsized && l.lines()[1].ellipsized);
    // Unwrapped, no line cap: one line per paragraph, ellipsized because it is too wide.
    l.set_max_lines(0);
    l.set_wrap(lt::text_wrap::none);
    LUMEN_CHECK(l.lines().size() == 1 && l.lines()[0].ellipsized);
}

LUMEN_TEST(ellipsis_at_the_logical_end_rtl) {
    if (skip_without(true, false, "ellipsis_at_the_logical_end_rtl"))
        return;
    lt::text_layout l{};
    if (!make(l, "שלום עולם שלום עולם שלום עולם", "ellipsis rtl"))
        return;
    l.set_max_lines(1);
    l.set_ellipsize(true);
    l.set_max_width(60.f);
    const lt::layout_line& line = l.lines()[0];
    LUMEN_CHECK(line.rtl() && line.ellipsized);
    // RTL: the logical end is the visual left, so the ellipsis is the first run, and the
    // logical start (byte 0) is the rightmost glyph.
    const auto runs = l.runs(line);
    LUMEN_CHECK(runs[0].ellipsis);
    LUMEN_CHECK(runs[0].x == line.x);
    const auto v = visual_clusters(l, line);
    print_clusters("visual clusters", v);
    LUMEN_CHECK(!v.empty() && v.back() == 0);
    LUMEN_CHECK(line.width <= px(60));
    // Right-aligned at `start`: the content's right edge is the box's.
    LUMEN_CHECK(line.x + line.width == px(60));
}

// ── Alignment ──────────────────────────────────────────────────────────────────────

LUMEN_TEST(alignment_follows_the_paragraph_direction) {
    lt::text_layout l{};
    if (!make(l, "abc", "align"))
        return;
    l.set_max_width(200.f);
    const std::int32_t box = px(200);
    const std::int32_t w = l.lines()[0].width;
    LUMEN_CHECK(l.lines()[0].x == 0);
    l.set_align(lt::text_align::end);
    LUMEN_CHECK(l.lines()[0].x == box - w);
    l.set_align(lt::text_align::center);
    LUMEN_CHECK(l.lines()[0].x == (box - w) / 2);
    // The runs move with the line.
    LUMEN_CHECK(l.runs()[0].x == (box - w) / 2);
    if (skip_without(true, false, "alignment (rtl part)"))
        return;
    l.set_text("שלום");
    l.set_align(lt::text_align::start);
    const std::int32_t wr = l.lines()[0].width;
    LUMEN_CHECK(l.lines()[0].x == box - wr);
    l.set_align(lt::text_align::end);
    LUMEN_CHECK(l.lines()[0].x == 0);
    // Wider than the box: the start edge stays put and the line overflows toward its end —
    // an RTL line keeps its right edge on the box's and runs out past the left one, for
    // every alignment; an LTR line starts at 0.
    l.set_wrap(lt::text_wrap::none);
    l.set_max_width(5.f);
    const std::int32_t small = px(5);
    for (const lt::text_align a :
         {lt::text_align::start, lt::text_align::end, lt::text_align::center}) {
        l.set_align(a);
        const lt::layout_line& line = l.lines()[0];
        LUMEN_CHECK(line.width > small);
        LUMEN_CHECK(line.x == small - line.width && line.x < 0);
        LUMEN_CHECK(l.runs()[0].x + l.runs()[0].width <= small);
    }
    l.set_text("abc");
    for (const lt::text_align a :
         {lt::text_align::start, lt::text_align::end, lt::text_align::center}) {
        l.set_align(a);
        LUMEN_CHECK(l.lines()[0].width > small && l.lines()[0].x == 0);
    }
}

// ── Hit testing ────────────────────────────────────────────────────────────────────

LUMEN_TEST(hit_testing_round_trips_across_a_direction_boundary) {
    if (skip_without(true, true, "hit_testing_round_trips_across_a_direction_boundary"))
        return;
    lt::text_layout l{};
    // abc␠ גבא ␠def on screen (see ltr_paragraph_with_an_embedded_hebrew_word).
    if (!make(l, "abc אבג def", "hit test"))
        return;
    const lt::layout_line& line = l.lines()[0];
    const std::int32_t y = line.top + line.height / 2;
    const lt::layout_run* heb = nullptr;
    for (const lt::layout_run& r : l.runs(line))
        if (r.begin == 4)
            heb = &r;
    LUMEN_CHECK(heb != nullptr);
    if (!heb)
        return;
    const std::int32_t left = heb->x;
    const std::int32_t right = heb->x + heb->width;
    // Position 4 (before א): downstream, the caret is on א's leading edge — its right side,
    // the Hebrew run's right edge; upstream, on the trailing edge of the space before it —
    // that space's right side, the Hebrew run's left edge.
    LUMEN_CHECK(l.caret(4, false).x == right);
    LUMEN_CHECK(l.caret(4, true).x == left);
    // Position 10 (after ג): upstream on ג's trailing (left) edge, downstream on the
    // following space's leading (left) edge, which is the run's right edge.
    LUMEN_CHECK(l.caret(10, true).x == left);
    LUMEN_CHECK(l.caret(10, false).x == right);
    // A point just inside the run's left edge is over ג's left half: its trailing side, so
    // position 10 upstream; just inside the right edge, א's right half: position 4.
    const lt::text_hit a = l.hit_test(left + 64, y);
    LUMEN_CHECK(a.index == 10 && a.upstream);
    const lt::text_hit b = l.hit_test(right - 64, y);
    LUMEN_CHECK(b.index == 4 && !b.upstream);
    // Round trips: every caret stop, both affinities. caret → hit_test → caret lands on the
    // same x; where both neighbours of the position are in one run, on the same position.
    unsigned exact = 0;
    for (std::uint32_t i = 0; i <= l.text().size(); i = l.next_caret(i)) {
        for (const bool up : {false, true}) {
            const lt::caret_pos c = l.caret(i, up);
            const lt::text_hit h = l.hit_test(c.x, y);
            const lt::caret_pos back = l.caret(h.index, h.upstream);
            if (back.x != c.x)
                std::printf("    position %u%s: x %d → hit %u%s → x %d\n", i, up ? "^" : "",
                            c.x, h.index, h.upstream ? "^" : "", back.x);
            LUMEN_CHECK(back.x == c.x);
            const bool interior = i != 0 && i != 4 && i != 10 && i != 14 && i != 3 && i != 11;
            if (interior) {
                LUMEN_CHECK(h.index == i);
                ++exact;
            }
        }
        if (i == l.text().size())
            break;
    }
    LUMEN_CHECK(exact > 0);
    // Outside the line: the visual ends.
    LUMEN_CHECK(l.hit_test(-px(10), y).index == 0);
    const lt::text_hit end = l.hit_test(line.x + line.width + px(10), y);
    LUMEN_CHECK(end.index == 14 && end.upstream);
}

LUMEN_TEST(soft_line_breaks_have_two_caret_positions) {
    if (skip_without(false, true, "soft_line_breaks_have_two_caret_positions"))
        return;
    lt::text_layout l{};
    if (!make(l, "alpha beta", "soft break"))
        return;
    lt::shaped_line s{};
    LUMEN_CHECK(lt::shape_line("alpha beta", lt::font_desc{}, s).ok());
    // Room for "alpha" but not "alpha beta": the break is after the space, at 6.
    l.set_max_width(static_cast<float>((width_before(s, 5) + width_before(s, 10)) / 2.0));
    LUMEN_CHECK(l.lines().size() == 2);
    LUMEN_CHECK(l.lines()[1].begin == 6);
    // Downstream: the start of line 1; upstream: the end of line 0 (after the space).
    const lt::caret_pos down = l.caret(6, false);
    const lt::caret_pos up = l.caret(6, true);
    LUMEN_CHECK(down.line == 1 && down.x == l.lines()[1].x);
    LUMEN_CHECK(up.line == 0 && up.x > l.lines()[0].x + l.lines()[0].width - 1);
}

// ── Clusters are never split (UAX #29) ─────────────────────────────────────────────

LUMEN_TEST(combining_marks_and_emoji_zwj_sequences_are_never_split) {
    if (skip_without(false, true, "combining_marks_and_emoji_zwj_sequences_are_never_split"))
        return;
    lt::text_layout l{};
    // e + U+0301 COMBINING ACUTE ACCENT, three times: three bytes per grapheme. GB9 (×
    // Extend) keeps each accent with its e; LB9 attaches CM to its base and LB28 (AL × AL)
    // forbids breaks between the letters, so a too-narrow line breaks at grapheme
    // boundaries only: [0,3) [3,6) [6,9).
    if (!make(l, "e\xCC\x81"
                 "e\xCC\x81"
                 "e\xCC\x81",
              "marks"))
        return;
    LUMEN_CHECK(l.next_caret(0) == 3 && l.next_caret(3) == 6 && l.prev_caret(6) == 3);
    LUMEN_CHECK(!l.is_caret_stop(1) && !l.is_caret_stop(2));
    l.set_max_width(1.f);
    auto lines = l.lines();
    LUMEN_CHECK(lines.size() == 3);
    for (unsigned i = 0; i < lines.size() && i < 3; ++i)
        LUMEN_CHECK(lines[i].begin == i * 3 && lines[i].end == i * 3 + 3);
    for (const lt::layout_run& r : l.runs())
        LUMEN_CHECK(l.is_caret_stop(r.begin) && l.is_caret_stop(r.end));

    // 👨‍👩‍👧: U+1F468 ZWJ U+1F469 ZWJ U+1F467 (4 + 3 + 4 + 3 + 4 = 18 bytes). GB11
    // (ExtPict Extend* ZWJ × ExtPict) makes it one grapheme; LB8a (ZWJ ×) leaves no break
    // inside. No colour-emoji font here: the glyphs are whatever face has them (or .notdef),
    // but the sequence stays one cluster, one run, one line.
    l.set_text("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7");
    LUMEN_CHECK(l.next_caret(0) == 18);
    LUMEN_CHECK(l.prev_caret(18) == 0);
    for (std::uint32_t b = 1; b < 18; ++b)
        LUMEN_CHECK(!l.is_caret_stop(b) && !l.break_opportunity(b));
    lines = l.lines();
    LUMEN_CHECK(lines.size() == 1 && lines[0].begin == 0 && lines[0].end == 18);
    LUMEN_CHECK(l.clusters().size() == 1);
    // With text around it at 1 px: "a", the family, "b" each on a line of its own.
    l.set_text("a\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
               "b");
    lines = l.lines();
    LUMEN_CHECK(lines.size() == 3);
    if (lines.size() == 3)
        LUMEN_CHECK(lines[1].begin == 1 && lines[1].end == 19);
}

// ── The retained layout ────────────────────────────────────────────────────────────

LUMEN_TEST(layout_is_invalidated_only_by_its_inputs) {
    lt::text_layout l{};
    if (!make(l, "Retained text, wrapped and rewrapped without shaping again", "retained"))
        return;
    auto& shapes = lt::shape_cache::shared();
    l.set_max_width(200.f);
    LUMEN_CHECK(l.update().ok());
    const lt::text_layout_counters c0 = l.counters();
    const std::uint64_t calls0 = shapes.counters().shaping_calls;
    // The same width, text and font again: nothing runs.
    l.set_max_width(200.f);
    l.set_text("Retained text, wrapped and rewrapped without shaping again");
    l.set_font({});
    l.set_scale(1.f);
    LUMEN_CHECK(!l.dirty());
    LUMEN_CHECK(l.update().ok());
    const lt::text_layout_counters c1 = l.counters();
    LUMEN_CHECK(c1.shape_passes == c0.shape_passes && c1.wrap_passes == c0.wrap_passes);
    LUMEN_CHECK(c1.shape_requests == c0.shape_requests);
    LUMEN_CHECK(shapes.counters().shaping_calls == calls0);
    // A new width re-wraps without shaping.
    l.set_max_width(120.f);
    LUMEN_CHECK(l.update().ok());
    const lt::text_layout_counters c2 = l.counters();
    LUMEN_CHECK(c2.shape_passes == c0.shape_passes);
    LUMEN_CHECK(c2.wrap_passes == c0.wrap_passes + 1);
    LUMEN_CHECK(c2.shape_requests == c0.shape_requests);
    LUMEN_CHECK(shapes.counters().shaping_calls == calls0);
    LUMEN_CHECK(l.lines().size() >= 2);
    // A width that rounds to the same 26.6 value is no change.
    l.set_max_width(120.001f);
    LUMEN_CHECK(!l.dirty());
    // Scale reshapes; scaling back reshapes from the cache (no shaping calls).
    l.set_scale(1.5f);
    LUMEN_CHECK(l.update().ok());
    LUMEN_CHECK(l.counters().shape_passes == c0.shape_passes + 1);
    const std::uint64_t calls1 = shapes.counters().shaping_calls;
    l.set_scale(1.f);
    LUMEN_CHECK(l.update().ok());
    LUMEN_CHECK(l.counters().shape_passes == c0.shape_passes + 2);
    LUMEN_CHECK(shapes.counters().shaping_calls == calls1);
    // Text changes reshape.
    l.set_text("Different");
    LUMEN_CHECK(l.dirty());
    LUMEN_CHECK(l.update().ok());
    LUMEN_CHECK(l.counters().shape_passes == c0.shape_passes + 3);
    std::printf("    storage %zu bytes\n", l.storage_bytes());
}

LUMEN_TEST(attributes_split_runs_and_reach_them) {
    lt::text_layout l{};
    if (!make(l, "plain bold link", "attrs"))
        return;
    const lt::text_attr attrs[] = {
        {.begin = 6, .end = 10, .weight = lt::weight::bold},
        {.begin = 11,
         .end = 15,
         .color = 0x3366FFFFu,
         .underline = lt::text_underline::single,
         .role = lt::span_role::link,
         .tag = 7},
    };
    l.set_attrs(attrs);
    LUMEN_CHECK(l.update().ok());
    // The same spans again (the normalised form, or the input as given) are no change.
    const std::uint64_t generation = l.shape_generation();
    l.set_attrs(attrs);
    const std::vector<lt::text_attr> normalised(l.attrs().begin(), l.attrs().end());
    l.set_attrs(normalised);
    LUMEN_CHECK(l.shape_generation() == generation && !l.dirty());
    // Out of order is sorted first: still equal, still no change.
    const lt::text_attr reversed[] = {attrs[1], attrs[0]};
    l.set_attrs(reversed);
    LUMEN_CHECK(l.shape_generation() == generation && !l.dirty());
    bool saw_bold = false;
    bool saw_link = false;
    for (const lt::layout_run& r : l.runs()) {
        if (r.attr == lt::k_no_attr)
            continue;
        const lt::text_attr& a = l.attrs()[r.attr];
        if (a.weight == lt::weight::bold) {
            saw_bold = true;
            LUMEN_CHECK(r.begin >= 6 && r.end <= 10);
        }
        if (a.role == lt::span_role::link) {
            saw_link = true;
            LUMEN_CHECK(a.tag == 7 && a.underline == lt::text_underline::single);
            LUMEN_CHECK(r.begin >= 11 && r.end <= 15);
        }
    }
    LUMEN_CHECK(saw_bold && saw_link);
    // Metrics for decorations.
    const lt::face_metrics* m = l.metrics(l.base_face());
    LUMEN_CHECK(m && m->underline_thickness >= 64 && m->underline_offset > 0);
}

LUMEN_TEST(caps_keep_what_fits_or_reject) {
    lt::text_layout l{};
    if (!make(l, "one\ntwo\nthree\nfour\nfive", "caps"))
        return;
    // drop_newest: what fits is kept and flagged; reject: the pass fails.
    lt::text_layout_limits lim{};
    lim.max_lines = 2;
    lim.overflow = lx::runtime::overflow_action::drop_newest;
    l.set_limits(lim);
    LUMEN_CHECK(l.update().ok());
    LUMEN_CHECK(l.lines().size() == 2 && l.truncated());
    lim.max_lines = lt::k_layout_max_lines;
    lim.max_glyphs = 6; // "one" + "two"
    l.set_limits(lim);
    LUMEN_CHECK(l.update().ok());
    LUMEN_CHECK(l.truncated());
    LUMEN_CHECK(l.lines().size() == 2);
    lim.overflow = lx::runtime::overflow_action::reject;
    l.set_limits(lim);
    LUMEN_CHECK(!l.update().ok());
    LUMEN_CHECK(l.lines().empty());
    // Text over the byte cap is cut at a code point boundary.
    lt::text_layout_limits bytes{};
    bytes.max_bytes = 4;
    bytes.overflow = lx::runtime::overflow_action::drop_newest;
    lt::text_layout b{};
    b.set_limits(bytes);
    b.set_text("ab\xC3\xA4\xC3\xB6"); // "abäö": the cap falls inside ö
    LUMEN_CHECK(b.text().size() == 4 && b.truncated());
}

LUMEN_TEST(legacy_wrapped_text_is_unchanged) {
    // The shell's wrap_text keeps its own path (ADR: shm_canvas::text stays until Phase 5).
    lt::wrapped_text w{};
    if (!lt::wrap_text("one two three four five six", {}, lt::font_desc{}, 60, 8, w)) {
        std::printf("    SKIP: no font face on this host\n");
        return;
    }
    LUMEN_CHECK(w.line_count >= 2);
}

// ── Measurement (printed, not gated) ───────────────────────────────────────────────

LUMEN_TEST(measure_s4_like_layout) {
    // An S4-like wall (ADR §8.3): 2 000 lines of about 50 characters, Latin / Cyrillic /
    // Arabic, each its own paragraph, over a private engine whose shape cache holds it all.
    static lt::face_cache faces{};
    static lt::shape_cache shapes{lt::shape_cache_config{.entries = 16384,
                                                         .glyph_capacity = 1u << 20}};
    static lt::itemizer items{};
    const lt::text_engine engine{&faces, &shapes, &items};
    if (!faces.resolve(lt::font_desc{}).valid()) {
        std::printf("    SKIP: no font face on this host\n");
        return;
    }
    const char* parts[] = {"The quick brown fox jumps over the lazy dog ",
                           "Съешь же ещё этих мягких французских булок ",
                           "العدد هنا والنص العربي مع الأرقام ", "line "};
    std::string text;
    for (int i = 0; i < 2000; ++i) {
        text += parts[i % 3];
        text += parts[3];
        text += std::to_string(i);
        text += '\n';
    }
    text.pop_back();
    using clock = std::chrono::steady_clock;
    const auto us = [](clock::duration d) {
        return std::chrono::duration<double, std::micro>(d).count();
    };
    lt::text_layout cold{engine};
    cold.set_text(text);
    cold.set_max_width(400.f);
    auto t0 = clock::now();
    LUMEN_CHECK(cold.update().ok());
    const double cold_us = us(clock::now() - t0);
    std::size_t glyphs = 0;
    for (const lt::layout_run& r : cold.runs())
        glyphs += r.glyph_count;
    // Warm: the same text in a new layout — itemized again, every run a cache hit.
    std::vector<double> warm;
    std::vector<double> rewrap;
    for (int rep = 0; rep < 9; ++rep) {
        lt::text_layout l{engine};
        l.set_text(text);
        l.set_max_width(400.f);
        const std::uint64_t calls = shapes.counters().shaping_calls;
        t0 = clock::now();
        LUMEN_CHECK(l.update().ok());
        warm.push_back(us(clock::now() - t0));
        LUMEN_CHECK(shapes.counters().shaping_calls == calls);
        for (int k = 0; k < 4; ++k) {
            l.set_max_width(k % 2 ? 400.f : 300.f);
            t0 = clock::now();
            LUMEN_CHECK(l.update().ok());
            rewrap.push_back(us(clock::now() - t0));
        }
        LUMEN_CHECK(shapes.counters().shaping_calls == calls);
    }
    // Itemization alone (fribidi, scripts, UAX #14 / #29, fallback lookups), every paragraph.
    std::vector<double> itemize;
    const lt::face_id base = faces.resolve(lt::font_desc{});
    for (int rep = 0; rep < 5; ++rep) {
        t0 = clock::now();
        auto lock = faces.lock();
        for (std::size_t at = 0; at < text.size();) {
            std::size_t nl = text.find('\n', at);
            if (nl == std::string::npos)
                nl = text.size();
            lt::itemize_request req{};
            req.face = base;
            req.utf8 = std::string_view{text}.substr(at, nl - at);
            LUMEN_CHECK(items.itemize_locked(faces, req).ok());
            at = nl + 1;
        }
        itemize.push_back(us(clock::now() - t0));
    }
    std::sort(itemize.begin(), itemize.end());
    std::sort(warm.begin(), warm.end());
    std::sort(rewrap.begin(), rewrap.end());
    const auto pct = [](const std::vector<double>& v, double p) {
        return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5)];
    };
    std::printf("    S4-like: %zu bytes, %zu lines, %zu glyphs, %zu KiB layout storage\n",
                text.size(), cold.lines().size(), glyphs, cold.storage_bytes() / 1024);
    std::printf("    cold shape pass + wrap %.0f us; warm (cache hits) p50 %.0f p95 %.0f us; "
                "re-wrap p50 %.0f p95 %.0f us\n",
                cold_us, pct(warm, 0.5), pct(warm, 0.95), pct(rewrap, 0.5), pct(rewrap, 0.95));
    std::printf("    of which itemization p50 %.0f us\n", pct(itemize, 0.5));
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
