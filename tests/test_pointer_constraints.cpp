// pointer-constraints' policy (design-app-compat-capture C.3, PC1), Wayland-free: the
// effective region (region ∩ input region ∩ surface box), confinement of a motion across
// multi-rect regions (crossing into a neighbour, sliding along an edge, never out), the
// nearest point inside, the activation predicate, and when a lock's cursor hint is used.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.compositor;

using namespace lx::compositor;

namespace {

input_region rects(std::initializer_list<lx::rect2i> list) {
    input_region r{};
    r.reset_empty();
    for (const auto& x : list)
        r.add(x);
    return r;
}

bool near(constraint_point p, double x, double y) {
    return std::fabs(p.x - x) < 1e-6 && std::fabs(p.y - y) < 1e-6;
}

constexpr double e = k_constraint_edge;

} // namespace

LUMEN_TEST(effective_region_is_region_and_input_and_box) {
    const input_region infinite{};
    // No region, infinite input: the surface box.
    auto r = make_constraint_region(nullptr, infinite, {200, 100});
    LUMEN_CHECK(r.count == 1 && r.rects[0].width == 200 && r.rects[0].height == 100);
    LUMEN_CHECK(r.contains(0, 0) && r.contains(199.9, 99.9) && !r.contains(200, 50) &&
                !r.contains(-0.1, 10));
    // A region partly outside the surface is clipped to it.
    const input_region reg = rects({{150, 50, 100, 100}});
    r = make_constraint_region(&reg, infinite, {200, 100});
    LUMEN_CHECK(r.count == 1 && r.rects[0].x == 150 && r.rects[0].width == 50 &&
                r.rects[0].height == 50);
    // ∩ the input region.
    const input_region input = rects({{0, 0, 160, 100}});
    r = make_constraint_region(&reg, input, {200, 100});
    LUMEN_CHECK(r.count == 1 && r.rects[0].x == 150 && r.rects[0].width == 10);
    // Disjoint: empty (never activates).
    const input_region far = rects({{300, 300, 10, 10}});
    LUMEN_CHECK(make_constraint_region(&far, infinite, {200, 100}).empty());
    // A surface without content: empty.
    LUMEN_CHECK(make_constraint_region(nullptr, infinite, {0, 0}).empty());
    // An empty (non-null) region: empty, not "everything".
    const input_region none = rects({});
    LUMEN_CHECK(make_constraint_region(&none, infinite, {200, 100}).empty());
}

LUMEN_TEST(effective_region_overflow_only_confines_more) {
    input_region a{};
    a.reset_empty();
    input_region b{};
    b.reset_empty();
    for (int i = 0; i < 8; ++i) {
        a.add({i * 20, 0, 10, 400});
        b.add({0, i * 20, 400, 10});
    }
    const auto r = make_constraint_region(&a, b, {400, 400});
    // 64 intersections, 32 kept: each kept one is a real part of the intersection.
    LUMEN_CHECK(r.count == constraint_region::k_max && r.approximated);
    for (unsigned i = 0; i < r.count; ++i)
        LUMEN_CHECK(r.rects[i].width == 10 && r.rects[i].height == 10);
}

LUMEN_TEST(confine_inside_one_rect_and_clamp_at_its_edges) {
    const input_region infinite{};
    const auto r = make_constraint_region(nullptr, infinite, {100, 80});
    LUMEN_CHECK(near(confine_motion(r, {10, 10}, {50, 40}), 50, 40));
    // Past the right edge: stops one wl_fixed step short of it, y moves on (slides).
    LUMEN_CHECK(near(confine_motion(r, {90, 10}, {130, 30}), 100 - e, 30));
    // Past the corner: both clamped.
    LUMEN_CHECK(near(confine_motion(r, {50, 40}, {500, 500}), 100 - e, 80 - e));
    // Past the left/top edges: exactly the edge (half-open rects include it).
    LUMEN_CHECK(near(confine_motion(r, {5, 5}, {-20, -40}), 0, 0));
    // A purely vertical motion along the edge keeps sliding.
    LUMEN_CHECK(near(confine_motion(r, {100 - e, 10}, {100 - e, 70}), 100 - e, 70));
    // Every result is inside.
    for (int i = 0; i < 64; ++i) {
        const double ang = i * 0.2;
        const auto p = confine_motion(r, {50, 40}, {50 + 300 * std::cos(ang), 40 + 300 * std::sin(ang)});
        LUMEN_CHECK(r.contains(p.x, p.y));
    }
}

LUMEN_TEST(confine_crosses_into_a_neighbouring_rect) {
    // An L: a wide bar on top, a narrow leg down the left.
    const input_region l = rects({{0, 0, 200, 50}, {0, 50, 50, 150}});
    const input_region infinite{};
    const auto r = make_constraint_region(&l, infinite, {200, 200});
    // From the bar straight down into the leg.
    LUMEN_CHECK(near(confine_motion(r, {20, 10}, {20, 180}), 20, 180));
    // From the leg up and right into the bar.
    LUMEN_CHECK(near(confine_motion(r, {20, 150}, {20, 10}), 20, 10));
    // From the bar's right part down: blocked at the bar's bottom, it slides no further (the
    // motion has no x component left).
    LUMEN_CHECK(near(confine_motion(r, {150, 10}, {150, 150}), 150, 50 - e));
    // Diagonal from the bar's right part towards the leg's bottom: x reaches the leg's column,
    // the leg is entered, never the empty quadrant.
    for (int i = 0; i < 40; ++i) {
        const auto p = confine_motion(r, {150, 10}, {10 + i * 5.0, 190});
        LUMEN_CHECK(r.contains(p.x, p.y));
    }
    // From the leg to the right: blocked at the leg's right edge, y slides.
    LUMEN_CHECK(near(confine_motion(r, {20, 150}, {120, 160}), 50 - e, 160));
}

LUMEN_TEST(confine_handles_overlapping_rects_and_a_start_outside) {
    // Overlapping rects (wl_region add keeps them so).
    const input_region ov = rects({{0, 0, 100, 100}, {50, 50, 100, 100}});
    const input_region infinite{};
    const auto r = make_constraint_region(&ov, infinite, {200, 200});
    LUMEN_CHECK(near(confine_motion(r, {10, 10}, {140, 140}), 140, 140));
    const auto q = confine_motion(r, {10, 10}, {190, 20});
    LUMEN_CHECK(r.contains(q.x, q.y) && near(q, 100 - e, 20));
    // A start outside (the region changed under the pointer) starts from the nearest point.
    const auto p = confine_motion(r, {180, 10}, {185, 10});
    LUMEN_CHECK(r.contains(p.x, p.y));
    // An empty region never moves the pointer.
    const constraint_region empty{};
    LUMEN_CHECK(near(confine_motion(empty, {3, 4}, {30, 40}), 3, 4));
}

LUMEN_TEST(nearest_inside_picks_the_closest_rect) {
    const input_region two = rects({{0, 0, 10, 10}, {100, 0, 10, 10}});
    const input_region infinite{};
    const auto r = make_constraint_region(&two, infinite, {200, 200});
    LUMEN_CHECK(near(nearest_inside(r, {5, 5}), 5, 5));
    LUMEN_CHECK(near(nearest_inside(r, {30, 5}), 10 - e, 5));
    LUMEN_CHECK(near(nearest_inside(r, {80, 50}), 100, 10 - e));
    LUMEN_CHECK(near(nearest_inside(r, {-5, -5}), 0, 0));
}

LUMEN_TEST(activation_needs_both_foci_and_the_pointer_inside) {
    constraint_conditions c{};
    c.pointer_focus = true;
    c.keyboard_focus = true;
    c.pointer_inside = true;
    LUMEN_CHECK(constraint_wanted(c, false));
    // Outside the region: does not activate, but an active one stays.
    c.pointer_inside = false;
    LUMEN_CHECK(!constraint_wanted(c, false) && constraint_wanted(c, true));
    c.pointer_inside = true;
    // Each blocker, alone, deactivates.
    for (int i = 0; i < 7; ++i) {
        constraint_conditions d = c;
        switch (i) {
        case 0: d.pointer_focus = false; break;
        case 1: d.keyboard_focus = false; break;
        case 2: d.session_locked = true; break;
        case 3: d.paused = true; break;
        case 4: d.grab = true; break;
        case 5: d.escaped = true; break;
        case 6: d.defunct = true; break;
        }
        LUMEN_CHECK(!constraint_wanted(d, false) && !constraint_wanted(d, true));
    }
}

LUMEN_TEST(cursor_hint_is_used_only_inside_the_surface) {
    const input_region input = rects({{0, 0, 100, 50}});
    const auto area = make_constraint_region(nullptr, input, {100, 100});
    LUMEN_CHECK(constraint_hint_usable(area, {10, 10}));
    LUMEN_CHECK(!constraint_hint_usable(area, {10, 70}));   // outside the input region
    LUMEN_CHECK(!constraint_hint_usable(area, {120, 10}));  // outside the surface
    LUMEN_CHECK(!constraint_hint_usable(area, {-1, 10}));
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
