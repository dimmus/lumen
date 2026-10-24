#include "lumen_test.hpp"

#include <stdlib.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.text;

// Regression: shaping must never dereference a null hb_font_t. When no face can be
// resolved the metric estimate is used instead, so this path must stay crash-free
// whether or not fonts are installed.
LUMEN_TEST(shape_ascii_is_positive_width) {
    lx::text::font_stack stack{};
    const auto run = stack.shape("hello", {});
    LUMEN_CHECK(run.width > 0.f);
    LUMEN_CHECK(run.height > 0.f);
}

LUMEN_TEST(shape_empty_text_is_zero) {
    lx::text::font_stack stack{};
    const auto empty = stack.shape("", {});
    LUMEN_CHECK(empty.width == 0.f);
    const auto null_run = stack.shape(nullptr, {});
    LUMEN_CHECK(null_run.width == 0.f);
}

LUMEN_TEST(shape_utf8_multibyte_does_not_overcount) {
    lx::text::font_stack stack{};
    lx::text::font_desc desc{};
    // "äöü" is 6 bytes but 3 characters — a byte-wise estimate would be 2x too wide.
    const auto accented = stack.shape("\xC3\xA4\xC3\xB6\xC3\xBC", desc);
    const auto ascii = stack.shape("abcdef", desc);
    LUMEN_CHECK(accented.width < ascii.width);
}

LUMEN_TEST(measure_width_matches_shape) {
    lx::text::font_stack stack{};
    lx::text::font_desc desc{};
    LUMEN_CHECK(stack.measure_width("lumen", desc) == stack.shape("lumen", desc).width);
}

LUMEN_TEST(text_layout_reports_bounding_size) {
    lx::text::text_layout layout{};
    layout.set_text("compositor");
    const auto size = layout.bounding_size();
    LUMEN_CHECK(size.width > 0);
    LUMEN_CHECK(size.height > 0);
}

LUMEN_TEST(text_layout_respects_max_width) {
    lx::text::text_layout layout{};
    layout.set_text("a very long line of text that should be clamped");
    layout.set_max_width(20.f);
    LUMEN_CHECK(layout.bounding_size().width <= 20);
}

LUMEN_TEST(repeated_shaping_is_stable) {
    lx::text::font_stack stack{};
    lx::text::font_desc desc{};
    const float first = stack.shape("stability", desc).width;
    for (int i = 0; i < 64; ++i)
        LUMEN_CHECK(stack.shape("stability", desc).width == first);
}

// ── Faces (lx.text:faces) ─────────────────────────────────────────────────────

namespace {

bool starts_with(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

bool ends_with(const char* s, const char* suffix) {
    const std::size_t n = std::strlen(s);
    const std::size_t m = std::strlen(suffix);
    return n >= m && std::strcmp(s + n - m, suffix) == 0;
}

/// Restores LUMEN_FONT_DIR when the test ends.
struct font_dir_env {
    std::string saved;
    bool had = false;
    explicit font_dir_env(const char* value) {
        if (const char* v = ::getenv("LUMEN_FONT_DIR")) {
            saved = v;
            had = true;
        }
        ::setenv("LUMEN_FONT_DIR", value, 1);
    }
    ~font_dir_env() {
        if (had)
            ::setenv("LUMEN_FONT_DIR", saved.c_str(), 1);
        else
            ::unsetenv("LUMEN_FONT_DIR");
    }
};

/// A copy of a run's glyphs (the run itself is valid only until the next shaping call).
std::vector<lx::text::shaped_glyph> copy_of(const lx::text::shaped_run& run) {
    return {run.glyphs.begin(), run.glyphs.end()};
}

bool same_glyphs(const std::vector<lx::text::shaped_glyph>& a,
                 std::span<const lx::text::shaped_glyph> b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].glyph != b[i].glyph || a[i].cluster != b[i].cluster ||
            a[i].x_advance != b[i].x_advance || a[i].x_offset != b[i].x_offset ||
            a[i].y_offset != b[i].y_offset)
            return false;
    return true;
}

/// DejaVu Sans covers Latin, Cyrillic and Arabic on every host this runs on; invalid without
/// HarfBuzz/FreeType or fonts, in which case the shaping tests have nothing to check.
lx::text::face_id dejavu() {
    return lx::text::face_cache::shared().resolve("DejaVu Sans", lx::text::weight::normal,
                                                  lx::text::style::normal);
}

} // namespace

LUMEN_TEST(default_family_is_the_bundled_ui_family) {
    LUMEN_CHECK(std::strcmp(lx::text::font_desc{}.family, lx::text::k_ui_family) == 0);
    LUMEN_CHECK(std::strcmp(lx::text::k_ui_family, "IBM Plex Sans") == 0);
    LUMEN_CHECK(std::strcmp(lx::text::k_mono_family, "Lilex") == 0);
    // FreeType's own rounding: 10 pt at 96 dpi is 13.33 px.
    LUMEN_CHECK(lx::text::size_26_6_from_pt(10.f) == (640 * 96 + 36) / 72);
    LUMEN_CHECK(lx::text::size_26_6_from_pt(0.f) == 0);
}

LUMEN_TEST(bundled_family_resolves_to_the_bundled_file) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id ui = faces.resolve(lx::text::font_desc{});
    if (!ui.valid())
        return; // no HarfBuzz/FreeType in this build
    const char* dir = faces.font_dir();
    std::printf("    font dir: \"%s\"\n", dir);
    if (!::getenv("LUMEN_FONT_DIR"))
        LUMEN_CHECK(dir[0] != '\0'); // the build tree's assets/fonts
    if (!dir[0])
        return;
    struct expect {
        const char* family;
        lx::text::weight w;
        lx::text::style s;
        const char* matched;
        const char* file;
    };
    const expect cases[] = {
        {lx::text::k_ui_family, lx::text::weight::normal, lx::text::style::normal, "IBM Plex Sans",
         "/IBMPlexSans-Regular.ttf"},
        {lx::text::k_ui_family, lx::text::weight::bold, lx::text::style::normal, "IBM Plex Sans",
         "/IBMPlexSans-Bold.ttf"},
        {lx::text::k_ui_family, lx::text::weight::normal, lx::text::style::italic, "IBM Plex Sans",
         "/IBMPlexSans-Italic.ttf"},
        {lx::text::k_mono_family, lx::text::weight::normal, lx::text::style::normal, "Lilex",
         "/Lilex-Regular.ttf"},
        {lx::text::k_mono_family, lx::text::weight::bold, lx::text::style::normal, "Lilex",
         "/Lilex-Bold.ttf"},
    };
    for (const expect& e : cases) {
        const lx::text::face_id id = faces.resolve(e.family, e.w, e.s);
        lx::text::face_info info{};
        LUMEN_CHECK(id.valid() && faces.describe(id, info));
        std::printf("    %s → %s (%s)\n", e.family, info.file, info.family);
        LUMEN_CHECK(std::strcmp(info.family, e.matched) == 0);
        LUMEN_CHECK(starts_with(info.file, dir));
        LUMEN_CHECK(ends_with(info.file, e.file));
    }
    // Asking for "Sans" still gets the system's sans-serif: the bundle adds families, it does
    // not replace fontconfig's aliases (the shell and the title bars ask for "Sans").
    lx::text::face_info sans{};
    LUMEN_CHECK(faces.describe(faces.resolve("Sans", lx::text::weight::normal,
                                             lx::text::style::normal),
                               sans));
    LUMEN_CHECK(!starts_with(sans.file, dir));
    // The same face again is the same id, loaded once.
    const auto before = faces.stats();
    LUMEN_CHECK(faces.resolve(lx::text::font_desc{}) == ui);
    const auto after = faces.stats();
    LUMEN_CHECK(after.loads == before.loads && after.hits == before.hits + 1);
}

LUMEN_TEST(missing_font_dir_falls_back_to_fontconfig) {
    const font_dir_env env{"/nonexistent/lumen-fonts"};
    lx::text::face_cache local{};
    const lx::text::face_id ui =
        local.resolve(lx::text::k_ui_family, lx::text::weight::normal, lx::text::style::normal);
    if (!ui.valid())
        return;
    LUMEN_CHECK(local.font_dir()[0] == '\0');
    lx::text::face_info info{};
    LUMEN_CHECK(local.describe(ui, info));
    std::printf("    %s → %s (%s)\n", lx::text::k_ui_family, info.file, info.family);
    LUMEN_CHECK(std::strcmp(info.requested_family, lx::text::k_ui_family) == 0);
    LUMEN_CHECK(std::strcmp(info.family, "IBM Plex Sans") != 0);
    LUMEN_CHECK(std::strstr(info.file, "/assets/fonts/") == nullptr);
    const lx::text::face_id mono =
        local.resolve(lx::text::k_mono_family, lx::text::weight::normal, lx::text::style::normal);
    LUMEN_CHECK(mono.valid() && local.describe(mono, info));
    std::printf("    %s → %s (%s)\n", lx::text::k_mono_family, info.file, info.family);
    LUMEN_CHECK(std::strcmp(info.family, "Lilex") != 0);
    // A fallback face is a working face.
    const auto m = local.metrics(ui, lx::text::size_26_6_from_pt(11.f));
    LUMEN_CHECK(m && m->ascent > 0 && m->descent > 0 && m->units_per_em > 0);
}

LUMEN_TEST(face_lru_evicts_least_recent_and_never_reuses_ids) {
    lx::text::face_cache local{""}; // system fonts only
    constexpr unsigned n = lx::text::k_face_cache_size + 1;
    char names[n + 1][32];
    for (unsigned i = 0; i <= n; ++i)
        std::snprintf(names[i], sizeof(names[i]), "lumen-test-face-%02u", i);
    const auto resolve = [&](unsigned i) {
        return local.resolve(names[i], lx::text::weight::normal, lx::text::style::normal);
    };
    std::vector<lx::text::face_id> ids;
    for (unsigned i = 0; i < lx::text::k_face_cache_size; ++i)
        ids.push_back(resolve(i));
    if (!ids[0].valid())
        return;
    auto st = local.stats();
    LUMEN_CHECK(st.live == lx::text::k_face_cache_size && st.evictions == 0 &&
                st.loads == lx::text::k_face_cache_size);
    // Face 0 is used again: face 1 is now the least recently used.
    LUMEN_CHECK(resolve(0) == ids[0]);
    ids.push_back(resolve(lx::text::k_face_cache_size));
    LUMEN_CHECK(!local.alive(ids[1]));
    LUMEN_CHECK(local.alive(ids[0]) && local.alive(ids.back()));
    // Back again: loaded anew (evicting face 2) under an id never handed out before.
    const lx::text::face_id again = resolve(1);
    LUMEN_CHECK(again.valid() && local.alive(again) && !local.alive(ids[2]));
    for (const lx::text::face_id id : ids)
        LUMEN_CHECK(again != id);
    ids.push_back(again);
    for (std::size_t i = 0; i < ids.size(); ++i)
        for (std::size_t j = i + 1; j < ids.size(); ++j)
            LUMEN_CHECK(ids[i] != ids[j]);
    st = local.stats();
    LUMEN_CHECK(st.evictions == 2 && st.loads == lx::text::k_face_cache_size + 2 &&
                st.live == lx::text::k_face_cache_size);
    // A stale id is refused, not answered with another face.
    LUMEN_CHECK(!local.metrics(ids[1], 64 * 12));
    lx::text::face_info info{};
    LUMEN_CHECK(!local.describe(ids[1], info));
}

LUMEN_TEST(face_size_is_set_per_use) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id id = dejavu();
    if (!id.valid())
        return;
    const auto loads = faces.stats().loads;
    const auto small = faces.metrics(id, 12 * 64);
    const auto large = faces.metrics(id, 24 * 64);
    const auto small_again = faces.metrics(id, 12 * 64);
    LUMEN_CHECK(small && large && small_again);
    LUMEN_CHECK(large->ascent > small->ascent && large->x_height > small->x_height);
    LUMEN_CHECK(small->ascent == small_again->ascent && small->descent == small_again->descent);
    LUMEN_CHECK(small->x_height > 0 && small->x_height < small->ascent);
    LUMEN_CHECK(small->units_per_em == 2048); // DejaVu Sans
    // Shaping follows the size too (HarfBuzz is told the face changed): twice the size, twice
    // the advance, and the first size again gives the first result.
    lx::text::shape_cache cache{};
    lx::text::shape_request req{};
    req.face = id;
    req.utf8 = "Sizes";
    req.size_26_6 = 12 * 64;
    const std::int32_t at12 = cache.shape(faces, req)->x_advance;
    req.size_26_6 = 24 * 64;
    const std::int32_t at24 = cache.shape(faces, req)->x_advance;
    lx::text::shape_cache fresh{};
    req.size_26_6 = 12 * 64;
    LUMEN_CHECK(fresh.shape(faces, req)->x_advance == at12);
    LUMEN_CHECK(at24 > at12 * 2 - 64 && at24 < at12 * 2 + 64);
    // One face for both sizes: no load.
    LUMEN_CHECK(faces.stats().loads == loads);
}

// ── Shaping (lx.text:shape) ───────────────────────────────────────────────────

LUMEN_TEST(shape_cache_hits_the_second_identical_call) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id id = dejavu();
    if (!id.valid())
        return;
    lx::text::shape_cache cache{};
    lx::text::shape_request req{};
    req.face = id;
    req.size_26_6 = lx::text::size_26_6_from_pt(11.f);
    req.utf8 = "Lumen shapes this once";
    const auto first = cache.shape(faces, req);
    LUMEN_CHECK(first && !first->hit && first->stored && !first->truncated);
    LUMEN_CHECK(first->glyphs.size() == req.utf8.size() && first->x_advance > 0);
    LUMEN_CHECK(first->direction == lx::text::text_direction::ltr);
    LUMEN_CHECK(first->script == lx::text::make_tag("Latn"));
    const auto glyphs = copy_of(*first);
    const auto c1 = cache.counters();
    LUMEN_CHECK(c1.misses == 1 && c1.shaping_calls == 1 && c1.hits == 0 && c1.entries == 1);
    LUMEN_CHECK(c1.bytes_in_use == glyphs.size() * sizeof(lx::text::shaped_glyph));
    const auto second = cache.shape(faces, req);
    LUMEN_CHECK(second && second->hit && same_glyphs(glyphs, second->glyphs));
    LUMEN_CHECK(second->x_advance == first->x_advance);
    const auto c2 = cache.counters();
    LUMEN_CHECK(c2.shaping_calls == c1.shaping_calls && c2.hits == 1 && c2.misses == 1);
    // Equal bytes from another buffer are the same key.
    const std::string copy{req.utf8};
    req.utf8 = copy;
    LUMEN_CHECK(cache.shape(faces, req)->hit);
    LUMEN_CHECK(cache.counters().shaping_calls == 1);
    // Empty text: an empty run, nothing counted.
    req.utf8 = {};
    const auto empty = cache.shape(faces, req);
    LUMEN_CHECK(empty && empty->glyphs.empty() && cache.counters().misses == 1);
    // A stale face id or no size is an error, not a guess.
    req.utf8 = "x";
    req.face = {};
    LUMEN_CHECK(!cache.shape(faces, req));
    req.face = id;
    req.size_26_6 = 0;
    LUMEN_CHECK(cache.shape(faces, req).get_error().domain == lx::error_domain::invalid_argument);
}

LUMEN_TEST(shape_cache_key_covers_size_direction_script_features_language_text) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id id = dejavu();
    if (!id.valid())
        return;
    lx::text::shape_cache cache{};
    lx::text::shape_request base{};
    base.face = id;
    base.size_26_6 = lx::text::size_26_6_from_pt(11.f);
    base.utf8 = "office affinity";
    const lx::text::font_feature no_liga[] = {{lx::text::make_tag("liga"), 0}};
    const lx::text::font_feature kern_off[] = {{lx::text::make_tag("kern"), 0}};
    lx::text::shape_request variants[8] = {base, base, base, base, base, base, base, base};
    variants[1].size_26_6 += 64;
    variants[2].direction = lx::text::text_direction::rtl;
    variants[3].script = lx::text::make_tag("Cyrl");
    variants[4].features = no_liga;
    variants[5].features = kern_off;
    variants[6].language = "tr";
    variants[7].utf8 = "office affinitz"; // same length, one byte apart
    for (const auto& v : variants) {
        const auto before = cache.counters().shaping_calls;
        const auto r = cache.shape(faces, v);
        LUMEN_CHECK(r && !r->hit);
        LUMEN_CHECK(cache.counters().shaping_calls == before + 1);
    }
    // Every variant is its own entry, each now a hit.
    for (const auto& v : variants)
        LUMEN_CHECK(cache.shape(faces, v)->hit);
    LUMEN_CHECK(cache.counters().entries == 8 && cache.counters().shaping_calls == 8);
    // The direction asked for is the direction shaped: RTL runs come out in visual order.
    const auto rtl = cache.shape(faces, variants[2]);
    LUMEN_CHECK(rtl->direction == lx::text::text_direction::rtl);
    LUMEN_CHECK(rtl->glyphs.front().cluster > rtl->glyphs.back().cluster);
}

LUMEN_TEST(shape_cache_arena_overflow_evicts_least_recently_used) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id id = dejavu();
    if (!id.valid())
        return;
    lx::text::shape_cache_config cfg{};
    cfg.entries = 64;
    cfg.glyph_capacity = 64;
    cfg.max_run_glyphs = 32;
    lx::text::shape_cache cache{cfg};
    // Five runs of 19 glyphs (digits: no ligatures); three fit the arena.
    char text[5][24];
    for (int i = 0; i < 5; ++i)
        std::snprintf(text[i], sizeof(text[i]), "%c123456789012345678", 'a' + i);
    const auto shape = [&](int i) {
        lx::text::shape_request req{};
        req.face = id;
        req.size_26_6 = 12 * 64;
        req.utf8 = text[i];
        return cache.shape(faces, req);
    };
    for (int i = 0; i < 3; ++i)
        LUMEN_CHECK(shape(i)->glyphs.size() == 19);
    LUMEN_CHECK(cache.counters().evictions == 0);
    LUMEN_CHECK(shape(3)->stored); // evicts run 0, the least recently used
    LUMEN_CHECK(cache.counters().evictions == 1);
    LUMEN_CHECK(cache.counters().compactions == 1);
    LUMEN_CHECK(shape(1)->hit); // run 2 is now the least recently used
    LUMEN_CHECK(shape(4)->stored);
    auto c = cache.counters();
    LUMEN_CHECK(c.evictions == 2 && c.entries == 3);
    LUMEN_CHECK(c.bytes_in_use == 3 * 19 * sizeof(lx::text::shaped_glyph));
    LUMEN_CHECK(c.bytes_in_use <= c.bytes_capacity);
    // Compaction moved the survivors; their glyphs are intact.
    const auto one = shape(1);
    LUMEN_CHECK(one->hit && one->glyphs.size() == 19 && one->glyphs[0].cluster == 0 &&
                one->glyphs[18].cluster == 18);
    LUMEN_CHECK(shape(3)->hit && shape(4)->hit);
    LUMEN_CHECK(!shape(2)->hit && !shape(0)->hit);

    // The entry table full: the least recently used entry goes, whatever the arena holds.
    lx::text::shape_cache_config two{};
    two.entries = 2;
    two.glyph_capacity = 1024;
    lx::text::shape_cache small{two};
    lx::text::shape_request req{};
    req.face = id;
    req.size_26_6 = 12 * 64;
    for (const char* t : {"one", "two", "one", "three"}) {
        req.utf8 = t;
        (void)small.shape(faces, req);
    }
    c = small.counters();
    LUMEN_CHECK(c.entries == 2 && c.evictions == 1 && c.hits == 1);
    req.utf8 = "one";
    LUMEN_CHECK(small.shape(faces, req)->hit);
    req.utf8 = "two";
    LUMEN_CHECK(!small.shape(faces, req)->hit);
}

LUMEN_TEST(shape_cache_drop_newest_keeps_the_cache_and_still_shapes) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id id = dejavu();
    if (!id.valid())
        return;
    lx::text::shape_cache_config cfg{};
    cfg.entries = 64;
    cfg.glyph_capacity = 40;
    cfg.max_run_glyphs = 32;
    cfg.overflow = lx::runtime::overflow_action::drop_newest;
    lx::text::shape_cache cache{cfg};
    lx::text::shape_cache reference{};
    lx::text::shape_request req{};
    req.face = id;
    req.size_26_6 = 12 * 64;
    const char* runs[] = {"a123456789012345678", "b123456789012345678", "c123456789012345678"};
    req.utf8 = runs[0];
    (void)cache.shape(faces, req);
    req.utf8 = runs[1];
    (void)cache.shape(faces, req);
    req.utf8 = runs[2];
    const auto dropped = cache.shape(faces, req);
    LUMEN_CHECK(dropped && !dropped->stored && !dropped->hit);
    const auto expected = reference.shape(faces, req);
    LUMEN_CHECK(same_glyphs(copy_of(*expected), dropped->glyphs));
    auto c = cache.counters();
    LUMEN_CHECK(c.overflows == 1 && c.evictions == 0 && c.entries == 2);
    // Not kept: shaped again next time; the old entries are untouched.
    LUMEN_CHECK(!cache.shape(faces, req)->hit);
    req.utf8 = runs[0];
    LUMEN_CHECK(cache.shape(faces, req)->hit);
    c = cache.counters();
    LUMEN_CHECK(c.overflows == 2 && c.shaping_calls == 4);

    // Longer than max_run_glyphs: cut at a cluster boundary and flagged.
    lx::text::shape_cache_config cut{};
    cut.max_run_glyphs = 8;
    lx::text::shape_cache short_runs{cut};
    req.utf8 = runs[1];
    const auto truncated = short_runs.shape(faces, req);
    LUMEN_CHECK(truncated && truncated->truncated && truncated->glyphs.size() == 8);
    LUMEN_CHECK(short_runs.shape(faces, req)->truncated);
}

LUMEN_TEST(single_line_shaping_goes_through_the_shared_cache) {
    const lx::text::font_desc font{"DejaVu Sans", 10.f, lx::text::weight::normal,
                                   lx::text::style::normal};
    static lx::text::shaped_line first{};
    static lx::text::shaped_line second{};
    if (!lx::text::shape_line("A title shaped once", font, first))
        return;
    const auto calls = [] {
        auto lock = lx::text::face_cache::shared().lock();
        return lx::text::shape_cache::shared().counters().shaping_calls;
    };
    const auto before = calls();
    LUMEN_CHECK(static_cast<bool>(lx::text::shape_line("A title shaped once", font, second)));
    LUMEN_CHECK(calls() == before);
    LUMEN_CHECK(second.count == first.count && second.width_px == first.width_px &&
                second.ascent_px == first.ascent_px);
    for (unsigned i = 0; i < first.count; ++i)
        LUMEN_CHECK(first.glyph[i] == second.glyph[i] && first.x_adv[i] == second.x_adv[i]);
    // font_stack measures through the same cache.
    lx::text::font_stack stack{};
    const float w = stack.measure_width("A title shaped once", font);
    LUMEN_CHECK(calls() == before);
    LUMEN_CHECK(static_cast<int>(w + 0.999f) == first.width_px);
}

// Not a gate: prints what shaping costs on this host (ADR §4.6, S4 sizing).
LUMEN_TEST(shaping_cost_report) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id id = dejavu();
    if (!id.valid())
        return;
    struct sample {
        const char* name;
        const char* text; // 50 code points
    };
    const sample samples[] = {
        {"latin", "The quick brown fox jumps over the lazy dog again!"},
        {"cyrillic", "Съешь же ещё этих мягких французских булок, да вып"},
        {"arabic", "نص حكيم له سر قاطع وذو شأن عظيم مكتوب على ثوب أخضر"},
    };
    using clock = std::chrono::steady_clock;
    for (const sample& s : samples) {
        // 50 rotations of the run: 50 distinct keys of the same glyphs.
        std::vector<std::string> rotations;
        std::vector<std::size_t> starts;
        const std::string_view t{s.text};
        for (std::size_t i = 0; i < t.size(); ++i)
            if ((static_cast<unsigned char>(t[i]) & 0xC0u) != 0x80u)
                starts.push_back(i);
        for (std::size_t k : starts)
            rotations.push_back(std::string{t.substr(k)} + std::string{t.substr(0, k)});
        std::vector<double> miss_us;
        std::size_t glyphs = 0;
        for (int round = 0; round < 8; ++round) {
            lx::text::shape_cache cache{};
            for (const std::string& r : rotations) {
                lx::text::shape_request req{};
                req.face = id;
                req.size_26_6 = lx::text::size_26_6_from_pt(11.f);
                req.utf8 = r;
                const auto t0 = clock::now();
                const auto run = cache.shape(faces, req);
                const auto t1 = clock::now();
                LUMEN_CHECK(run && !run->hit);
                glyphs = run->glyphs.size();
                miss_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
        }
        std::sort(miss_us.begin(), miss_us.end());
        lx::text::shape_cache cache{};
        lx::text::shape_request req{};
        req.face = id;
        req.size_26_6 = lx::text::size_26_6_from_pt(11.f);
        req.utf8 = s.text;
        (void)cache.shape(faces, req);
        constexpr int hits = 20000;
        const auto h0 = clock::now();
        for (int i = 0; i < hits; ++i)
            LUMEN_CHECK(cache.shape(faces, req)->hit);
        const double hit_ns =
            std::chrono::duration<double, std::nano>(clock::now() - h0).count() / hits;
        std::printf("    %-8s %zu cp, %zu glyphs: miss p50 %.1f us p95 %.1f us; hit %.0f ns\n",
                    s.name, starts.size(), glyphs, miss_us[miss_us.size() / 2],
                    miss_us[miss_us.size() * 95 / 100], hit_ns);
    }
    // The single-line API on a cache hit: lock, face lookup, metrics, copy into shaped_line.
    static lx::text::shaped_line line{};
    const lx::text::font_desc font{"DejaVu Sans", 11.f, lx::text::weight::normal,
                                   lx::text::style::normal};
    (void)lx::text::shape_line(samples[0].text, font, line);
    constexpr int reps = 20000;
    const auto l0 = clock::now();
    for (int i = 0; i < reps; ++i)
        (void)lx::text::shape_line(samples[0].text, font, line);
    std::printf("    shape_line (cached, 50 glyphs): %.0f ns\n",
                std::chrono::duration<double, std::nano>(clock::now() - l0).count() / reps);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
