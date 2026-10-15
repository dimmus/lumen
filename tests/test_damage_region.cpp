#include "lumen_test.hpp"

import lx.foundation;
import lx.gfx;
import lx.scene;
import lx.runtime;

namespace {

bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool has_rect(const lx::gfx::damage_region& r, lx::rect2i want) {
    for (unsigned i = 0; i < r.count; ++i) {
        if (same(r.rects[i], want))
            return true;
    }
    return false;
}

} // namespace

LUMEN_TEST(damage_empty_and_degenerate_rects_are_ignored) {
    lx::gfx::damage_region r{};
    LUMEN_CHECK(r.empty());
    r.add({10, 10, 0, 5});
    r.add({10, 10, 5, -1});
    LUMEN_CHECK(r.empty());
    LUMEN_CHECK(r.area() == 0);
}

LUMEN_TEST(damage_contained_rect_is_absorbed_and_container_replaces) {
    lx::gfx::damage_region r{};
    r.add({0, 0, 100, 100});
    r.add({10, 10, 20, 20});
    LUMEN_CHECK(r.count == 1);
    LUMEN_CHECK(same(r.rects[0], {0, 0, 100, 100}));

    lx::gfx::damage_region s{};
    s.add({10, 10, 20, 20});
    s.add({500, 500, 10, 10});
    s.add({0, 0, 100, 100}); // swallows the first, not the second
    LUMEN_CHECK(s.count == 2);
    LUMEN_CHECK(has_rect(s, {0, 0, 100, 100}));
    LUMEN_CHECK(has_rect(s, {500, 500, 10, 10}));
}

LUMEN_TEST(damage_disjoint_window_move_stays_two_clips) {
    // A 300x200 window moved by (+400, +300): the bbox would be 700x500 for 120000 px of
    // real change. The region must keep the old and new rects apart.
    lx::gfx::damage_region r{};
    const lx::rect2i old_rect{100, 100, 300, 200};
    const lx::rect2i new_rect{500, 400, 300, 200};
    r.add(old_rect);
    r.add(new_rect);
    LUMEN_CHECK(r.count == 2);
    LUMEN_CHECK(has_rect(r, old_rect));
    LUMEN_CHECK(has_rect(r, new_rect));
    LUMEN_CHECK(r.area() == 120000ull);
    LUMEN_CHECK(same(r.bounds(), {100, 100, 700, 500}));
}

LUMEN_TEST(damage_small_move_merges_into_one_rect) {
    lx::gfx::damage_region r{};
    r.add({100, 100, 300, 200});
    r.add({103, 102, 300, 200});
    LUMEN_CHECK(r.count == 1);
    LUMEN_CHECK(same(r.rects[0], {100, 100, 303, 202}));
}

LUMEN_TEST(damage_merge_cascades_into_rects_it_now_covers) {
    lx::gfx::damage_region r{};
    r.add({0, 0, 10, 10});
    r.add({20, 0, 10, 10});
    LUMEN_CHECK(r.count == 2); // bbox 30x10 = 300 vs union 200: too wasteful
    r.add({8, 0, 14, 10});     // bridges the gap: bbox of all three is exactly their union
    LUMEN_CHECK(r.count == 1);
    LUMEN_CHECK(same(r.rects[0], {0, 0, 30, 10}));
}

LUMEN_TEST(damage_overflow_merges_the_cheapest_pair) {
    lx::gfx::damage_region r{};
    // Sixteen far-apart 10x10 rects along a row, 100 px apart.
    for (int i = 0; i < 16; ++i)
        r.add({i * 100, 0, 10, 10});
    LUMEN_CHECK(r.count == 16);
    // A seventeenth right next to the first: merging it with {0,0,10,10} wastes far less
    // than merging any two of the spaced ones.
    r.add({12, 0, 10, 10});
    LUMEN_CHECK(r.count == 16);
    LUMEN_CHECK(has_rect(r, {0, 0, 22, 10}));
    for (int i = 1; i < 16; ++i)
        LUMEN_CHECK(has_rect(r, {i * 100, 0, 10, 10}));
}

LUMEN_TEST(damage_overflow_without_new_rect_in_pair_keeps_it) {
    lx::gfx::damage_region r{};
    // Two stored rects 6 px apart — too wasteful to merge on their own (bbox 260 vs union
    // 200), but the cheapest pair once the region is full. The rest are far apart, and the
    // new rect is far from everything.
    r.add({0, 0, 10, 10});
    r.add({16, 0, 10, 10});
    for (int i = 1; i < 15; ++i)
        r.add({i * 100, 0, 10, 10});
    LUMEN_CHECK(r.count == 16);
    r.add({5000, 5000, 10, 10});
    LUMEN_CHECK(r.count == 16);
    LUMEN_CHECK(has_rect(r, {0, 0, 26, 10}));
    LUMEN_CHECK(has_rect(r, {5000, 5000, 10, 10}));
}

LUMEN_TEST(damage_clip_drops_outside_and_trims_edges) {
    lx::gfx::damage_region r{};
    r.add({-50, -50, 100, 100});
    r.add({2000, 2000, 10, 10});
    r.clip({0, 0, 1920, 1080});
    LUMEN_CHECK(r.count == 1);
    LUMEN_CHECK(same(r.rects[0], {0, 0, 50, 50}));
}

LUMEN_TEST(damage_full_propagates_and_clips_to_bounds) {
    lx::gfx::damage_region full{};
    full.set_full();
    LUMEN_CHECK(!full.empty());

    lx::gfx::damage_region r{};
    r.add({1, 1, 2, 2});
    r.add_region(full);
    LUMEN_CHECK(r.full);
    r.add({5, 5, 5, 5}); // no-op once full
    LUMEN_CHECK(r.full && r.count == 0);

    r.clip({0, 0, 640, 480});
    LUMEN_CHECK(!r.full);
    LUMEN_CHECK(r.count == 1);
    LUMEN_CHECK(same(r.rects[0], {0, 0, 640, 480}));
}

LUMEN_TEST(snapshot_carries_damage_of_an_unconsumed_snapshot_forward) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::snapshot_buffer buf{};

    lx::gfx::damage_region first{};
    first.add({0, 0, 10, 10});
    auto a = buf.begin_frame();
    LUMEN_CHECK(buf.publish(a, first, 1));

    // Published again before the consumer looked: frame 1's damage must not be lost.
    lx::gfx::damage_region second{};
    second.add({500, 500, 10, 10});
    auto b = buf.begin_frame();
    LUMEN_CHECK(buf.publish(b, second, 2));

    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = buf.acquire();
    LUMEN_CHECK(snap.frame_index() == 2);
    LUMEN_CHECK(snap.damage_rects().count == 2);
    LUMEN_CHECK(has_rect(snap.damage_rects(), {0, 0, 10, 10}));
    LUMEN_CHECK(has_rect(snap.damage_rects(), {500, 500, 10, 10}));
    buf.release(snap);

    // Consumed now, so the next publish carries only its own damage.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::gfx::damage_region third{};
    third.add({50, 50, 5, 5});
    auto c = buf.begin_frame();
    LUMEN_CHECK(buf.publish(c, third, 3));
    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap3 = buf.acquire();
    LUMEN_CHECK(snap3.damage_rects().count == 1);
    LUMEN_CHECK(has_rect(snap3.damage_rects(), {50, 50, 5, 5}));
    buf.release(snap3);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
