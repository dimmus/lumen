#include "lumen_test.hpp"

import lx.foundation;
import lx.gfx;

LUMEN_TEST(headless_blit_golden_pixel) {
    lx::gfx::headless_backend hb{};
    LUMEN_CHECK(static_cast<bool>(hb.create(4, 4)));

    const unsigned char red[] = {255, 0, 0, 255};
    hb.register_texture(42, 1, 1, red);

    LUMEN_CHECK(static_cast<bool>(hb.begin_frame()));
    LUMEN_CHECK(static_cast<bool>(hb.clear(lx::color::rgb(0.f, 0.f, 0.f))));
    LUMEN_CHECK(static_cast<bool>(hb.blit_texture(42, {0, 0, 4, 4})));
    LUMEN_CHECK(static_cast<bool>(hb.end_frame_and_dump(nullptr)));

    const unsigned char* px = hb.pixel_data();
    LUMEN_CHECK(px != nullptr);
    // Top-left pixel should be red after blit (RGB order in framebuffer).
    LUMEN_CHECK(px[0] == 255);
    LUMEN_CHECK(px[1] == 0);
    LUMEN_CHECK(px[2] == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
