// Persisted fontconfig answers (`lx.text:faces` `persist_matches`): a run that finds its faces
// in the match file loads them with FreeType alone and never starts fontconfig; anything an
// answer depends on that changed since (a watched path, the inputs) makes it ask again.
#include "lumen_test.hpp"

#include <stdlib.h>
#include <unistd.h>

import std;
import lx.text;

#ifndef LUMEN_ASSET_FONT_DIR
#define LUMEN_ASSET_FONT_DIR "assets/fonts"
#endif

namespace {

namespace fs = std::filesystem;

struct temp_dir {
    fs::path root;
    temp_dir() {
        char tmpl[] = "/tmp/lumen-font-matches-XXXXXX";
        root = ::mkdtemp(tmpl);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write_file(const fs::path& p, std::string_view text) {
    std::ofstream(p, std::ios::binary) << text;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

lx::text::font_match sample(const fs::path& file) {
    return {"IBM Plex Sans", lx::text::weight::bold, lx::text::style::normal, file.string(), 0,
            "IBM Plex Sans"};
}

} // namespace

LUMEN_TEST(font_matches_round_trip) {
    temp_dir t;
    const fs::path face = t.root / "face.ttf";
    write_file(face, "not a font, only a path to watch");
    const std::string watched[] = {face.string(), (t.root / "absent").string()};
    const lx::text::font_match m[] = {sample(face)};
    const std::string store = (t.root / "sub/dir/fonts-test").string();
    LUMEN_CHECK(lx::text::write_font_matches(store.c_str(), 42, watched, m));
    std::vector<lx::text::font_match> got;
    LUMEN_CHECK(lx::text::read_font_matches(store.c_str(), 42, got));
    LUMEN_CHECK(got.size() == 1);
    LUMEN_CHECK(got[0].family == "IBM Plex Sans" && got[0].weight == lx::text::weight::bold &&
                got[0].style == lx::text::style::normal && got[0].file == face.string() &&
                got[0].index == 0 && got[0].matched_family == "IBM Plex Sans");
    // Other inputs (fontconfig's version, the environment): nothing.
    LUMEN_CHECK(!lx::text::read_font_matches(store.c_str(), 43, got) && got.empty());
}

LUMEN_TEST(font_matches_reject_a_changed_path) {
    temp_dir t;
    const fs::path face = t.root / "face.ttf";
    const fs::path absent = t.root / "fonts.conf";
    write_file(face, "v1");
    const std::string watched[] = {face.string(), absent.string()};
    const lx::text::font_match m[] = {sample(face)};
    const std::string store = (t.root / "fonts-test").string();
    std::vector<lx::text::font_match> got;

    LUMEN_CHECK(lx::text::write_font_matches(store.c_str(), 1, watched, m));
    write_file(face, "v2, longer"); // size and mtime move
    LUMEN_CHECK(!lx::text::read_font_matches(store.c_str(), 1, got) && got.empty());

    LUMEN_CHECK(lx::text::write_font_matches(store.c_str(), 1, watched, m));
    write_file(absent, "<fontconfig/>"); // a configuration file appears
    LUMEN_CHECK(!lx::text::read_font_matches(store.c_str(), 1, got));

    LUMEN_CHECK(lx::text::write_font_matches(store.c_str(), 1, watched, m));
    fs::remove(face); // a face goes
    LUMEN_CHECK(!lx::text::read_font_matches(store.c_str(), 1, got));
}

LUMEN_TEST(font_matches_reject_what_they_cannot_hold) {
    temp_dir t;
    const std::string store = (t.root / "fonts-test").string();
    std::vector<lx::text::font_match> got;
    LUMEN_CHECK(!lx::text::read_font_matches(store.c_str(), 1, got)); // no file
    lx::text::font_match tab = sample(t.root / "face.ttf");
    tab.family = "Bad\tFamily";
    const lx::text::font_match bad[] = {tab};
    LUMEN_CHECK(!lx::text::write_font_matches(store.c_str(), 1, {}, bad));
    LUMEN_CHECK(!lx::text::write_font_matches("relative/path", 1, {}, {}));

    // Cut short, or a line it does not know: nothing.
    const lx::text::font_match ok[] = {sample(t.root / "face.ttf")};
    LUMEN_CHECK(lx::text::write_font_matches(store.c_str(), 1, {}, ok));
    const std::string text = read_file(store);
    write_file(store, text.substr(0, text.size() - 1));
    LUMEN_CHECK(!lx::text::read_font_matches(store.c_str(), 1, got) && got.empty());
    write_file(store, text + "x junk\n");
    LUMEN_CHECK(!lx::text::read_font_matches(store.c_str(), 1, got));
    write_file(store, text);
    LUMEN_CHECK(lx::text::read_font_matches(store.c_str(), 1, got) && got.size() == 1);
}

LUMEN_TEST(face_cache_answers_from_persisted_matches) {
    if (!fs::is_directory(LUMEN_ASSET_FONT_DIR))
        return;
    temp_dir t;
    // The bundled fonts in a directory of the test's own, so one can be changed.
    const fs::path fonts = t.root / "fonts";
    fs::copy(LUMEN_ASSET_FONT_DIR, fonts, fs::copy_options::recursive);
    const std::string store = (t.root / "cache/fonts-test").string();
    const auto resolve_ui = [](lx::text::face_cache& c) {
        return c.resolve(lx::text::k_ui_family, lx::text::weight::bold, lx::text::style::normal);
    };

    lx::text::face_info first{};
    {
        lx::text::face_cache cold{fonts.c_str()};
        cold.persist_matches(store.c_str());
        const lx::text::face_id id = resolve_ui(cold);
        if (!id.valid())
            return; // built without text support
        LUMEN_CHECK(cold.describe(id, first));
        const auto st = cold.stats();
        LUMEN_CHECK(st.fontconfig_started && st.stored_hits == 0);
        LUMEN_CHECK(fs::exists(store));
    }
    {
        // The next run: from the file, fontconfig never started.
        lx::text::face_cache warm{fonts.c_str()};
        warm.persist_matches(store.c_str());
        const lx::text::face_id id = resolve_ui(warm);
        LUMEN_CHECK(id.valid());
        lx::text::face_info info{};
        LUMEN_CHECK(warm.describe(id, info));
        LUMEN_CHECK(std::strcmp(info.file, first.file) == 0 && info.index == first.index &&
                    std::strcmp(info.family, first.family) == 0);
        const auto st = warm.stats();
        LUMEN_CHECK(!st.fontconfig_started && st.stored_hits == 1 && st.loads == 1);
        const auto m = warm.metrics(id, lx::text::size_26_6_from_pt(10.f));
        LUMEN_CHECK(m && m->ascent > 0);
        // A family the file does not have starts fontconfig, and joins the file.
        const lx::text::face_id mono = warm.resolve(lx::text::k_mono_family,
                                                    lx::text::weight::normal,
                                                    lx::text::style::normal);
        LUMEN_CHECK(mono.valid() && warm.stats().fontconfig_started);
    }
    {
        lx::text::face_cache both{fonts.c_str()};
        both.persist_matches(store.c_str());
        LUMEN_CHECK(resolve_ui(both).valid());
        LUMEN_CHECK(both.resolve(lx::text::k_mono_family, lx::text::weight::normal,
                                 lx::text::style::normal)
                        .valid());
        const auto st = both.stats();
        LUMEN_CHECK(!st.fontconfig_started && st.stored_hits == 2);
    }
    {
        // A font added to a bundled directory: every answer is asked again.
        write_file(fonts / "README.new", "a new file in the font tree");
        lx::text::face_cache changed{fonts.c_str()};
        changed.persist_matches(store.c_str());
        LUMEN_CHECK(resolve_ui(changed).valid());
        const auto st = changed.stats();
        LUMEN_CHECK(st.fontconfig_started && st.stored_hits == 0);
    }
    {
        // Not persisting: the file is not read.
        lx::text::face_cache plain{fonts.c_str()};
        LUMEN_CHECK(resolve_ui(plain).valid());
        LUMEN_CHECK(plain.stats().fontconfig_started && plain.stats().stored_hits == 0);
    }
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
