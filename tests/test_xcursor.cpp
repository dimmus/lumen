#include "lumen_test.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

import lx.foundation;
import lx.compositor;

using lx::compositor::cursor_shape;

namespace {

void put32(std::vector<unsigned char>& v, std::uint32_t x) {
    v.push_back(static_cast<unsigned char>(x));
    v.push_back(static_cast<unsigned char>(x >> 8));
    v.push_back(static_cast<unsigned char>(x >> 16));
    v.push_back(static_cast<unsigned char>(x >> 24));
}

struct image_spec {
    std::uint32_t nominal;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t xhot;
    std::uint32_t yhot;
    std::uint32_t fill;
};

/// A well-formed Xcursor file with one image chunk per spec.
std::vector<unsigned char> make_xcursor(const std::vector<image_spec>& images) {
    std::vector<unsigned char> f;
    f.insert(f.end(), {'X', 'c', 'u', 'r'});
    put32(f, 16);
    put32(f, 0x10000);
    put32(f, static_cast<std::uint32_t>(images.size()));
    std::uint32_t pos = 16 + 12 * static_cast<std::uint32_t>(images.size());
    for (const auto& im : images) {
        put32(f, 0xfffd0002u);
        put32(f, im.nominal);
        put32(f, pos);
        pos += 36 + im.width * im.height * 4;
    }
    for (const auto& im : images) {
        put32(f, 36);
        put32(f, 0xfffd0002u);
        put32(f, im.nominal);
        put32(f, 1);
        put32(f, im.width);
        put32(f, im.height);
        put32(f, im.xhot);
        put32(f, im.yhot);
        put32(f, 50);
        for (std::uint32_t i = 0; i < im.width * im.height; ++i)
            put32(f, im.fill);
    }
    return f;
}

bool write_file(const std::string& path, const std::vector<unsigned char>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

bool write_text(const std::string& path, const char* text) {
    const std::string s{text};
    return write_file(path, std::vector<unsigned char>(s.begin(), s.end()));
}

} // namespace

LUMEN_TEST(xcursor_picks_the_nearest_size_and_the_larger_on_a_tie) {
    const auto file = make_xcursor({{24, 24, 24, 3, 4, 0xFF112233u}, {48, 48, 48, 6, 8, 0xFF445566u}});
    std::vector<std::uint32_t> px;
    unsigned w = 0, h = 0;
    int hx = 0, hy = 0;
    LUMEN_CHECK(lx::compositor::parse_xcursor(file.data(), file.size(), 30, px, w, h, hx, hy));
    LUMEN_CHECK(w == 24 && h == 24 && hx == 3 && hy == 4);
    LUMEN_CHECK(px.size() == 24u * 24u && px[0] == 0xFF112233u && px.back() == 0xFF112233u);

    // 36 is 12 away from both: the larger wins.
    LUMEN_CHECK(lx::compositor::parse_xcursor(file.data(), file.size(), 36, px, w, h, hx, hy));
    LUMEN_CHECK(w == 48 && hx == 6 && hy == 8 && px[100] == 0xFF445566u);
}

LUMEN_TEST(xcursor_rejects_truncated_and_hostile_files) {
    const auto good = make_xcursor({{24, 24, 24, 0, 0, 0xFFFFFFFFu}});
    std::vector<std::uint32_t> px;
    unsigned w = 0, h = 0;
    int hx = 0, hy = 0;
    // Every truncation fails cleanly — including inside the pixel data.
    for (std::size_t len = 0; len < good.size(); len += 7)
        LUMEN_CHECK(!lx::compositor::parse_xcursor(good.data(), len, 24, px, w, h, hx, hy));

    auto bad_magic = good;
    bad_magic[0] = 'Y';
    LUMEN_CHECK(!lx::compositor::parse_xcursor(bad_magic.data(), bad_magic.size(), 24, px, w, h, hx, hy));

    // Oversized image (the 256 px cap) and a hotspot outside the image.
    const auto huge = make_xcursor({{24, 300, 2, 0, 0, 0}});
    LUMEN_CHECK(!lx::compositor::parse_xcursor(huge.data(), huge.size(), 24, px, w, h, hx, hy));
    const auto bad_hot = make_xcursor({{24, 8, 8, 9, 0, 0}});
    LUMEN_CHECK(!lx::compositor::parse_xcursor(bad_hot.data(), bad_hot.size(), 24, px, w, h, hx, hy));

    // A table of contents claiming more entries than the file holds.
    auto lying_toc = good;
    lying_toc[12] = 0xFF;
    lying_toc[13] = 0xFF;
    LUMEN_CHECK(!lx::compositor::parse_xcursor(lying_toc.data(), lying_toc.size(), 24, px, w, h, hx, hy));

    // A chunk offset pointing past the end.
    auto far_chunk = good;
    far_chunk[16 + 8] = 0xFF;
    far_chunk[16 + 9] = 0xFF;
    LUMEN_CHECK(!lx::compositor::parse_xcursor(far_chunk.data(), far_chunk.size(), 24, px, w, h, hx, hy));
}

LUMEN_TEST(xcursor_theme_follows_inherits_and_falls_back_to_builtins) {
    char tmpl[] = "/tmp/lumen-xcursor-XXXXXX";
    const char* root = mkdtemp(tmpl);
    LUMEN_CHECK(root != nullptr);
    const std::string dir{root};
    LUMEN_CHECK(mkdir((dir + "/child").c_str(), 0700) == 0);
    LUMEN_CHECK(mkdir((dir + "/parent").c_str(), 0700) == 0);
    LUMEN_CHECK(mkdir((dir + "/parent/cursors").c_str(), 0700) == 0);
    // A cycle back to the child must not loop.
    LUMEN_CHECK(write_text(dir + "/child/index.theme", "[Icon Theme]\nInherits=parent\n"));
    LUMEN_CHECK(write_text(dir + "/parent/index.theme", "[Icon Theme]\nInherits=child,missing\n"));
    // Found by its legacy X name.
    LUMEN_CHECK(write_file(dir + "/parent/cursors/left_ptr",
                           make_xcursor({{24, 20, 22, 1, 2, 0xFF00FF00u}})));

    lx::compositor::xcursor_theme theme;
    theme.set_search_path(dir.c_str());
    theme.load("child", 24);
    LUMEN_CHECK(theme.from_theme() == 1);
    LUMEN_CHECK(theme.builtin() == lx::compositor::k_cursor_shape_count - 2);

    const auto def = theme.image(cursor_shape::default_);
    LUMEN_CHECK(def.width == 20 && def.height == 22 && def.hot_x == 1 && def.hot_y == 2);
    LUMEN_CHECK(def.pixels[0] == 0xFF00FF00u);
    // Everything else is the built-in set, never empty.
    for (unsigned s = 1; s < lx::compositor::k_cursor_shape_count; ++s)
        LUMEN_CHECK(!theme.image(static_cast<cursor_shape>(s)).empty());
    LUMEN_CHECK(theme.max_dimension() >= 24);

    std::remove((dir + "/parent/cursors/left_ptr").c_str());
    std::remove((dir + "/parent/index.theme").c_str());
    std::remove((dir + "/child/index.theme").c_str());
    rmdir((dir + "/parent/cursors").c_str());
    rmdir((dir + "/parent").c_str());
    rmdir((dir + "/child").c_str());
    rmdir(dir.c_str());
}

LUMEN_TEST(xcursor_builtins_are_crisp_at_every_size) {
    for (unsigned s = 1; s < lx::compositor::k_cursor_shape_count; ++s) {
        std::vector<std::uint32_t> small, large;
        unsigned w = 0, h = 0, W = 0, H = 0;
        int hx = 0, hy = 0, HX = 0, HY = 0;
        lx::compositor::rasterize_builtin(static_cast<cursor_shape>(s), 24, small, w, h, hx, hy);
        lx::compositor::rasterize_builtin(static_cast<cursor_shape>(s), 48, large, W, H, HX, HY);
        LUMEN_CHECK(w == 24 && h == 24 && W == 48 && H == 48);
        LUMEN_CHECK(hx >= 0 && hy >= 0 && hx < 24 && hy < 24);
        LUMEN_CHECK(HX >= 0 && HY >= 0 && HX < 48 && HY < 48);
        // Rasterized, not scaled: the 48 px hotspot is twice the 24 px one (±1 rounding).
        LUMEN_CHECK(HX - 2 * hx <= 1 && 2 * hx - HX <= 1);
        unsigned opaque_black = 0, opaque_white = 0;
        for (const auto p : small) {
            // Premultiplied: no channel exceeds alpha.
            const unsigned a = p >> 24;
            LUMEN_CHECK(((p >> 16) & 0xFF) <= a && ((p >> 8) & 0xFF) <= a && (p & 0xFF) <= a);
            if (p == 0xFF000000u)
                ++opaque_black;
            if (p == 0xFFFFFFFFu)
                ++opaque_white;
        }
        LUMEN_CHECK(opaque_black > 0); // a solid interior
        LUMEN_CHECK(opaque_white > 0); // and an outline that shows on dark content
    }
}

LUMEN_TEST(xcursor_resize_edges_map_to_their_shapes) {
    using lx::compositor::shape_for_edges;
    LUMEN_CHECK(shape_for_edges(0) == cursor_shape::move);
    LUMEN_CHECK(shape_for_edges(1) == cursor_shape::n_resize);
    LUMEN_CHECK(shape_for_edges(2) == cursor_shape::s_resize);
    LUMEN_CHECK(shape_for_edges(4) == cursor_shape::w_resize);
    LUMEN_CHECK(shape_for_edges(8) == cursor_shape::e_resize);
    LUMEN_CHECK(shape_for_edges(5) == cursor_shape::nw_resize);
    LUMEN_CHECK(shape_for_edges(9) == cursor_shape::ne_resize);
    LUMEN_CHECK(shape_for_edges(6) == cursor_shape::sw_resize);
    LUMEN_CHECK(shape_for_edges(10) == cursor_shape::se_resize);
}

LUMEN_TEST(cursor_mailbox_hands_over_only_the_newest_value) {
    lx::compositor::cursor_frame_mailbox box;
    LUMEN_CHECK(!box.take());
    for (int i = 1; i <= 3; ++i) {
        box.back().x = i;
        box.publish();
    }
    LUMEN_CHECK(box.take());
    LUMEN_CHECK(box.front().x == 3);
    LUMEN_CHECK(!box.take()); // nothing newer
    LUMEN_CHECK(box.front().x == 3);
    box.back().x = 4;
    box.publish();
    LUMEN_CHECK(box.take() && box.front().x == 4);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
