// xdg-desktop-portal-lumen's pure parts (design-app-compat-capture Part E, F.1): frames
// turned upright with the compositor's own transform map (checked against lx.gfx's
// `transform_rect` for all eight), layout composition (1:1 copies, mixed scales, gaps, the
// largest-scale rule, fractional scales), colour sampling, PNG round trips, file naming and
// URIs, user-dirs.dirs, and xdg-desktop-portal-wlr's chooser lines.
#include "lumen_test.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.portal;

using namespace lx::portal;

namespace {

/// A content image whose every pixel is unique: 0xFF000000 | (y << 12) | x.
std::uint32_t code(int x, int y) { return 0xFF000000u | (static_cast<std::uint32_t>(y) << 12) | static_cast<std::uint32_t>(x); }

/// A solid shot.
output_shot solid(lx::rect2i logical, int pw, int ph, std::uint32_t c, const char* name = "OUT") {
    output_shot s;
    s.name = name;
    s.logical = logical;
    s.pixels.allocate(pw, ph);
    for (auto& p : s.pixels.pixels)
        p = c;
    return s;
}

std::string temp_dir() {
    char tmpl[] = "/tmp/lumen-portal-test-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "";
}

} // namespace

LUMEN_TEST(buffer_pixel_map_is_the_compositors_for_all_eight_transforms) {
    const int w = 5, h = 3;
    for (unsigned t = 0; t < 8; ++t) {
        const auto tr = static_cast<lx::buffer_transform>(t);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const lx::rect2i g = lx::gfx::transform_rect({x, y, 1, 1}, tr, w, h);
                const lx::point2i p = buffer_pixel_of(x, y, tr, w, h);
                LUMEN_CHECK(p.x == g.x && p.y == g.y);
            }
    }
}

LUMEN_TEST(upright_undoes_the_output_transform_with_padded_rows) {
    const int w = 7, h = 4; // layout orientation
    for (unsigned t = 0; t < 8; ++t) {
        const auto tr = static_cast<lx::buffer_transform>(t);
        const lx::size2i bs = (t & 1u) ? lx::size2i{h, w} : lx::size2i{w, h};
        const int stride = bs.width + 3;
        std::vector<std::uint32_t> buf(static_cast<std::size_t>(stride) * bs.height, 0xDEADBEEFu);
        // What the compositor does: content pixel (x, y) lands at transform_rect's pixel;
        // the padding byte of XRGB is garbage (0x00 here).
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const lx::rect2i g = lx::gfx::transform_rect({x, y, 1, 1}, tr, w, h);
                buf[static_cast<std::size_t>(g.y) * stride + g.x] = code(x, y) & 0x00FFFFFFu;
            }
        const image up = upright(buf.data(), bs.width, bs.height, stride, tr);
        LUMEN_CHECK(up.width == w && up.height == h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                LUMEN_CHECK(up.at(x, y) == code(x, y));
    }
    LUMEN_CHECK(upright(nullptr, 4, 4, 4, lx::buffer_transform::normal).empty());
}

LUMEN_TEST(one_output_at_its_own_scale_is_an_exact_copy_and_a_crop) {
    output_shot s;
    s.logical = {100, 50, 8, 6};
    s.pixels.allocate(8, 6);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x)
            s.pixels.pixels[static_cast<std::size_t>(y) * 8 + x] = code(x, y);
    const std::vector<output_shot> shots{s};
    LUMEN_CHECK(layout_bounds(shots).x == 100 && layout_bounds(shots).width == 8);
    const image all = compose(shots, layout_bounds(shots), 1.0);
    LUMEN_CHECK(all.width == 8 && all.height == 6 && all.pixels == s.pixels.pixels);
    const image crop = compose(shots, {102, 51, 3, 2}, 1.0);
    LUMEN_CHECK(crop.width == 3 && crop.height == 2);
    LUMEN_CHECK(crop.at(0, 0) == code(2, 1) && crop.at(2, 1) == code(4, 2));
    // Partly outside every output: black there.
    const image out = compose(shots, {98, 50, 4, 1}, 1.0);
    LUMEN_CHECK(out.at(0, 0) == 0xFF000000u && out.at(1, 0) == 0xFF000000u && out.at(2, 0) == code(0, 0));
}

LUMEN_TEST(mixed_scales_compose_at_the_largest_without_a_seam) {
    // Left: 100x50 logical at scale 1 (red). Right: 100x50 logical at scale 2 (green).
    const std::vector<output_shot> shots{solid({0, 0, 100, 50}, 100, 50, 0xFFFF0000u, "A"),
                                         solid({100, 0, 100, 50}, 200, 100, 0xFF00FF00u, "B")};
    const lx::rect2i all = layout_bounds(shots);
    LUMEN_CHECK(all.width == 200 && all.height == 50);
    LUMEN_CHECK(compose_scale(shots, all) == 2.0);
    // A region on the scale-1 output alone composes at 1.
    LUMEN_CHECK(compose_scale(shots, {10, 10, 20, 20}) == 1.0);
    const image img = compose(shots, all, 2.0);
    LUMEN_CHECK(img.width == 400 && img.height == 100);
    LUMEN_CHECK(img.at(0, 0) == 0xFFFF0000u && img.at(199, 99) == 0xFFFF0000u);
    LUMEN_CHECK(img.at(200, 0) == 0xFF00FF00u && img.at(399, 99) == 0xFF00FF00u);
    // A region straddling both at scale 1: the scale-2 side is sampled down.
    const image down = compose(shots, {90, 0, 20, 10}, 1.0);
    LUMEN_CHECK(down.width == 20 && down.at(9, 5) == 0xFFFF0000u && down.at(10, 5) == 0xFF00FF00u);
}

LUMEN_TEST(fractional_scale_output_copies_exactly_and_samples_between) {
    // 1.5: 640x360 logical shown with 960x540 pixels.
    output_shot s = solid({0, 0, 640, 360}, 960, 540, 0xFF102030u);
    s.pixels.pixels[0] = 0xFFFFFFFFu;
    const std::vector<output_shot> shots{s};
    LUMEN_CHECK(shot_scale(s) == 1.5);
    const image full = compose(shots, {0, 0, 640, 360}, compose_scale(shots, {0, 0, 640, 360}));
    LUMEN_CHECK(full.width == 960 && full.height == 540 && full.pixels == s.pixels.pixels);
    // An odd logical area at 1.5 lands on half pixels: sampled, still the solid colour
    // away from the corner.
    const image area = compose(shots, {101, 51, 33, 21}, 1.5);
    LUMEN_CHECK(area.width == 50 && area.height == 32);
    LUMEN_CHECK(area.at(25, 16) == 0xFF102030u);
}

LUMEN_TEST(colour_at_reads_the_shot_at_its_own_resolution) {
    output_shot s = solid({10, 10, 4, 4}, 8, 8, 0xFF000000u);
    s.pixels.pixels[static_cast<std::size_t>(3) * 8 + 5] = 0xFF00AA55u; // pixel (5,3)
    const std::vector<output_shot> shots{s};
    std::uint32_t c = 0;
    LUMEN_CHECK(colour_at(shots, 10 + 2.6, 10 + 1.6, c) && c == 0xFF00AA55u);
    LUMEN_CHECK(!colour_at(shots, 9.9, 10, c));
    LUMEN_CHECK(shot_at(shots, 13.99, 13.99) == 0 && shot_at(shots, 14, 13) == -1);
}

LUMEN_TEST(dim_scales_every_channel_and_keeps_alpha_opaque) {
    image img;
    img.allocate(1, 1);
    img.pixels[0] = 0xFFFF8040u;
    dim(img, 128);
    LUMEN_CHECK(img.pixels[0] == 0xFF7F4020u);
}

LUMEN_TEST(png_round_trip_keeps_every_pixel) {
    const std::string dir = temp_dir();
    LUMEN_CHECK(!dir.empty());
    image img;
    img.allocate(37, 23);
    for (int y = 0; y < 23; ++y)
        for (int x = 0; x < 37; ++x)
            img.pixels[static_cast<std::size_t>(y) * 37 + x] =
                0xFF000000u | (static_cast<std::uint32_t>(x * 7) << 16) | (static_cast<std::uint32_t>(y * 11) << 8) |
                static_cast<std::uint32_t>((x + y) & 0xFF);
    std::string path;
    lx::result<int> fd = create_unique(dir, "shot", true, path);
    LUMEN_CHECK(fd.ok());
    LUMEN_CHECK(write_png(fd.value(), img).ok());
    close(fd.value());
    struct stat st{};
    LUMEN_CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    lx::result<image> back = read_png(path.c_str());
    LUMEN_CHECK(back.ok());
    LUMEN_CHECK(back.value().width == 37 && back.value().height == 23 && back.value().pixels == img.pixels);
    unlink(path.c_str());
    rmdir(dir.c_str());
}

LUMEN_TEST(file_names_never_overwrite_and_old_private_files_are_pruned) {
    const std::string dir = temp_dir();
    std::string p1, p2, p3;
    lx::result<int> a = create_unique(dir, "Screenshot from x", false, p1);
    lx::result<int> b = create_unique(dir, "Screenshot from x", false, p2);
    lx::result<int> c = create_unique(dir, "Other", true, p3);
    LUMEN_CHECK(a.ok() && b.ok() && c.ok());
    close(a.value());
    close(b.value());
    close(c.value());
    LUMEN_CHECK(p1 == dir + "/Screenshot from x.png" && p2 == dir + "/Screenshot from x-2.png");
    // Make the first one two hours old: only it goes.
    timeval tv[2]{};
    tv[0].tv_sec = tv[1].tv_sec = std::time(nullptr) - 7200;
    utimes(p1.c_str(), tv);
    LUMEN_CHECK(prune_older(dir, "Screenshot", 3600) == 1);
    LUMEN_CHECK(access(p1.c_str(), F_OK) != 0 && access(p2.c_str(), F_OK) == 0 && access(p3.c_str(), F_OK) == 0);
    unlink(p2.c_str());
    unlink(p3.c_str());
    // make_dirs creates the chain.
    const std::string deep = dir + "/a/b/c";
    LUMEN_CHECK(make_dirs(deep, 0700).ok());
    struct stat st{};
    LUMEN_CHECK(stat(deep.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    rmdir(deep.c_str());
    rmdir((dir + "/a/b").c_str());
    rmdir((dir + "/a").c_str());
    rmdir(dir.c_str());
    LUMEN_CHECK(!make_dirs("relative/dir", 0700).ok());
}

LUMEN_TEST(uris_names_and_user_dirs) {
    LUMEN_CHECK(file_uri("/home/u/Pictures/Screenshots/Screenshot from 2026-10-22 13-45-07.png") ==
                "file:///home/u/Pictures/Screenshots/Screenshot%20from%202026-10-22%2013-45-07.png");
    LUMEN_CHECK(file_uri("/tmp/a#b%c\xC3\xA9") == "file:///tmp/a%23b%25c%C3%A9");
    std::tm t{};
    t.tm_year = 126;
    t.tm_mon = 9;
    t.tm_mday = 22;
    t.tm_hour = 3;
    t.tm_min = 4;
    t.tm_sec = 5;
    LUMEN_CHECK(screenshot_stem(t) == "Screenshot from 2026-10-22 03-04-05");
    const char* text = "# comment\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\n  XDG_PICTURES_DIR=\"$HOME/My \\\"Pics\\\"/\"\n"
                       "XDG_MUSIC_DIR=\"/srv/music\"\n";
    LUMEN_CHECK(parse_user_dir(text, "XDG_PICTURES_DIR", "/home/u") == "/home/u/My \"Pics\"");
    LUMEN_CHECK(parse_user_dir(text, "XDG_MUSIC_DIR", "/home/u") == "/srv/music");
    LUMEN_CHECK(parse_user_dir(text, "XDG_VIDEOS_DIR", "/home/u").empty());
    LUMEN_CHECK(parse_user_dir("XDG_PICTURES_DIR=\"relative\"\n", "XDG_PICTURES_DIR", "/h").empty());
    // pictures_dir: the file, else the variable, else ~/Pictures.
    const std::string cfg = temp_dir();
    LUMEN_CHECK(pictures_dir("/home/u", cfg.c_str(), nullptr) == "/home/u/Pictures");
    LUMEN_CHECK(pictures_dir("/home/u", cfg.c_str(), "/data/pics") == "/data/pics");
    const std::string file = cfg + "/user-dirs.dirs";
    if (FILE* f = std::fopen(file.c_str(), "w")) {
        std::fputs("XDG_PICTURES_DIR=\"$HOME/Bilder\"\n", f);
        std::fclose(f);
    }
    LUMEN_CHECK(pictures_dir("/home/u", cfg.c_str(), "/data/pics") == "/home/u/Bilder");
    unlink(file.c_str());
    rmdir(cfg.c_str());
}

LUMEN_TEST(chooser_lines_from_xdg_desktop_portal_wlr) {
    const auto entries = parse_chooser_input(
        "Monitor: Virtual-1 VMware Inc Virtual monitor\r\n"
        "Window: foot (deadbeef00112233)\n\n"
        "Window: Build (release) - log (0123456789abcdef)\n"
        "Window:  (cafe)\n"
        "Monitor: HDMI-A-1\n"
        "something else\n");
    LUMEN_CHECK(entries.size() == 6);
    LUMEN_CHECK(entries[0].type == chooser_entry::kind::monitor && entries[0].title == "Virtual-1" &&
                entries[0].detail == "VMware Inc Virtual monitor" &&
                entries[0].line == "Monitor: Virtual-1 VMware Inc Virtual monitor");
    LUMEN_CHECK(entries[1].type == chooser_entry::kind::window && entries[1].title == "foot" &&
                entries[1].detail == "deadbeef00112233");
    LUMEN_CHECK(entries[2].title == "Build (release) - log" && entries[2].detail == "0123456789abcdef");
    LUMEN_CHECK(entries[3].title == "Untitled window" && entries[3].detail == "cafe");
    LUMEN_CHECK(entries[4].title == "HDMI-A-1" && entries[4].detail.empty());
    LUMEN_CHECK(entries[5].type == chooser_entry::kind::other && entries[5].line == "something else");
    LUMEN_CHECK(parse_chooser_input("a\nb\nc\n", 2).size() == 2);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
