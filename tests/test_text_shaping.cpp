#include "lumen_test.hpp"

import lx.foundation;
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

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
