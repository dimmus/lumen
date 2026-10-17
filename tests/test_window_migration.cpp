// Window migration when an output goes (`lx.compositor:wm` `evacuate_rect`, ADR 008 §B.5):
// a window keeps its relative position in the fallback output's usable area — at the left
// edge it stays at the left edge, at the right edge at the right edge, in between in
// proportion — keeps its size when it fits and is shrunk to the area when it does not, and
// always ends inside it. The return home and the protocol side are covered in-process by
// test_hotplug_contract.
#include "lumen_test.hpp"

import lx.foundation;
import lx.compositor;

using lx::compositor::evacuate_rect;

namespace {
[[nodiscard]] bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
constexpr lx::rect2i k_from{1920, 32, 1280, 992}; // a second output under a 32 px panel
constexpr lx::rect2i k_to{0, 32, 1918, 896};
} // namespace

LUMEN_TEST(migration_edges_stay_edges) {
    // Left / top edge.
    LUMEN_CHECK(same(evacuate_rect({1920, 32, 640, 480}, k_from, k_to), {0, 32, 640, 480}));
    // Right / bottom edge.
    LUMEN_CHECK(same(evacuate_rect({1920 + 640, 32 + 512, 640, 480}, k_from, k_to),
                     {1918 - 640, 32 + 896 - 480, 640, 480}));
}

LUMEN_TEST(migration_relative_position) {
    // Half-way across the free room (640 of 1280 − 640 → x offset 320): half-way in the new
    // room (1918 − 640 = 1278 → 639).
    const lx::rect2i r = evacuate_rect({1920 + 320, 32 + 256, 640, 480}, k_from, k_to);
    LUMEN_CHECK(r.x == 639);
    LUMEN_CHECK(r.y == 32 + 208); // 256 of 512 → 208 of 416
    LUMEN_CHECK(r.width == 640 && r.height == 480);
}

LUMEN_TEST(migration_too_large_is_shrunk) {
    const lx::rect2i r = evacuate_rect({1920, 32, 1280, 992}, k_from, {0, 0, 1024, 768});
    LUMEN_CHECK(same(r, {0, 0, 1024, 768}));
    // Only one axis too large.
    const lx::rect2i h = evacuate_rect({1920 + 100, 32, 800, 992}, k_from, {0, 0, 1024, 768});
    LUMEN_CHECK(h.width == 800 && h.height == 768 && h.y == 0);
    LUMEN_CHECK(h.x >= 0 && h.x + h.width <= 1024);
}

LUMEN_TEST(migration_partly_off_its_output) {
    // Hanging off the left edge of the area it was in (a frame dragged half out): clamped
    // to the edge, then placed.
    LUMEN_CHECK(evacuate_rect({1800, 0, 640, 480}, k_from, k_to).x == 0);
    LUMEN_CHECK(evacuate_rect({3500, 2000, 640, 480}, k_from, k_to).x == 1918 - 640);
    // As wide as the old area: no room to keep a fraction of — at the new area's start.
    const lx::rect2i r = evacuate_rect({1920, 32, 1280, 400}, k_from, k_to);
    LUMEN_CHECK(r.x == 0 && r.width == 1280);
    // Whatever goes in, the result is inside the target.
    for (int x = -3000; x <= 6000; x += 750) {
        const lx::rect2i q = evacuate_rect({x, x / 3, 500, 300}, k_from, k_to);
        LUMEN_CHECK(q.x >= k_to.x && q.x + q.width <= k_to.x + k_to.width);
        LUMEN_CHECK(q.y >= k_to.y && q.y + q.height <= k_to.y + k_to.height);
    }
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
