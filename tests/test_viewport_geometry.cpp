// wl_surface + wp_viewport geometry (`lx.compositor:viewport`) and the output projection
// (`lx.scene::project_node` / `project_damage`): surface size for every combination of
// scale, transform, source and destination; the commit-time checks (bad_size,
// out_of_buffer, invalid_size); damage conversion in both directions for all eight
// transforms; edge rounding at fractional scales, including the pixel-exactness property a
// fractional-scale client relies on. Pure functions — no Wayland, no GPU.
#include "lumen_test.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

import lx.foundation;
import lx.scene;
import lx.compositor;

using lx::compositor::buffer_to_surface_damage;
using lx::compositor::check_buffer_scale;
using lx::compositor::check_viewport;
using lx::compositor::compute_geometry;
using lx::compositor::representable_scale;
using lx::compositor::surface_geometry_in;
using lx::compositor::surface_size_error;
using lx::compositor::surface_to_buffer_damage;
using lx::compositor::viewport_error;
using lx::scene::output_projection;
using lx::scene::project_damage;
using lx::scene::project_node;

namespace {

constexpr std::int32_t fx(double v) { return static_cast<std::int32_t>(v * 256.0); }

surface_geometry_in buffer(int w, int h, int scale = 1,
                           lx::buffer_transform t = lx::buffer_transform::normal) {
    surface_geometry_in in{};
    in.buffer_w = w;
    in.buffer_h = h;
    in.scale = scale;
    in.transform = t;
    return in;
}

surface_geometry_in& source(surface_geometry_in& in, double x, double y, double w, double h) {
    in.has_src = true;
    in.src_x = fx(x);
    in.src_y = fx(y);
    in.src_w = fx(w);
    in.src_h = fx(h);
    return in;
}

surface_geometry_in& destination(surface_geometry_in& in, int w, int h) {
    in.has_dst = true;
    in.dst_w = w;
    in.dst_h = h;
    return in;
}

bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool covers(lx::rect2i outer, lx::rect2i inner) {
    return outer.x <= inner.x && outer.y <= inner.y &&
           outer.x + outer.width >= inner.x + inner.width &&
           outer.y + outer.height >= inner.y + inner.height;
}

} // namespace

LUMEN_TEST(surface_size_follows_scale_transform_and_viewport) {
    // No buffer: 0x0 whatever else is set.
    {
        surface_geometry_in in{};
        destination(in, 100, 50);
        LUMEN_CHECK(compute_geometry(in).size.empty());
    }
    // Buffer only.
    LUMEN_CHECK(compute_geometry(buffer(640, 480)).size.width == 640);
    // Scale divides.
    {
        const auto g = compute_geometry(buffer(640, 480, 2));
        LUMEN_CHECK(g.size.width == 320 && g.size.height == 240);
        LUMEN_CHECK(g.src.empty()); // the whole buffer
    }
    // 90 and 270 (and their flips) swap the sides; 180 does not.
    for (unsigned t = 0; t < 8; ++t) {
        const auto g = compute_geometry(buffer(640, 480, 2, static_cast<lx::buffer_transform>(t)));
        const bool quarter = (t & 1u) != 0;
        LUMEN_CHECK(g.size.width == (quarter ? 240 : 320));
        LUMEN_CHECK(g.size.height == (quarter ? 320 : 240));
    }
    // A source alone sets the size (integers, else bad_size).
    {
        auto in = buffer(640, 480, 2);
        source(in, 10, 20, 100, 50);
        const auto g = compute_geometry(in);
        LUMEN_CHECK(g.size.width == 100 && g.size.height == 50);
        // In buffer pixels: the source times the scale.
        LUMEN_CHECK(g.src.x == 20.f && g.src.y == 40.f && g.src.width == 200.f &&
                    g.src.height == 100.f);
    }
    // A destination wins over the source; a fractional source is fine with one.
    {
        auto in = buffer(640, 480);
        source(in, 0.5, 0.25, 100.5, 50.75);
        destination(in, 300, 200);
        const auto g = compute_geometry(in);
        LUMEN_CHECK(g.size.width == 300 && g.size.height == 200);
        LUMEN_CHECK(g.src.x == 0.5f && g.src.y == 0.25f && g.src.width == 100.5f &&
                    g.src.height == 50.75f);
    }
    // Destination without source: the whole buffer scaled to it (fractional-scale clients).
    {
        auto in = buffer(1949, 983);
        destination(in, 1559, 786);
        const auto g = compute_geometry(in);
        LUMEN_CHECK(g.size.width == 1559 && g.size.height == 786);
        LUMEN_CHECK(g.src.empty());
    }
}

LUMEN_TEST(source_box_turns_into_the_buffer) {
    // Buffer 200x100 at transform 90: the surface sees it as 100x200, at scale 2 as 50x100.
    // Source (10, 20, 30x40) is (20, 40, 60x80) in the turned space; turned back by 270 it
    // is (40, 20, 80x60) in the buffer (wlroots' wlr_surface_get_buffer_source_box).
    auto in = buffer(200, 100, 2, lx::buffer_transform::rotate_90);
    source(in, 10, 20, 30, 40);
    const auto g = compute_geometry(in);
    LUMEN_CHECK(g.size.width == 30 && g.size.height == 40);
    LUMEN_CHECK(g.src.x == 40.f && g.src.y == 20.f && g.src.width == 80.f &&
                g.src.height == 60.f);
    LUMEN_CHECK(g.transform == lx::buffer_transform::rotate_90);
    // 180: mirrored on both axes.
    auto in2 = buffer(200, 100, 1, lx::buffer_transform::rotate_180);
    source(in2, 10, 20, 30, 40);
    const auto g2 = compute_geometry(in2);
    LUMEN_CHECK(g2.src.x == 160.f && g2.src.y == 40.f && g2.src.width == 30.f &&
                g2.src.height == 40.f);
}

LUMEN_TEST(viewport_checks_bad_size_and_out_of_buffer) {
    // bad_size: a fractional source without a destination; fine with one.
    {
        auto in = buffer(100, 100);
        source(in, 0, 0, 10.5, 10);
        LUMEN_CHECK(check_viewport(in) == viewport_error::bad_size);
        destination(in, 20, 20);
        LUMEN_CHECK(check_viewport(in) == viewport_error::none);
    }
    // An integer source is fine without a destination; a fractional origin is too.
    {
        auto in = buffer(100, 100);
        source(in, 0.5, 0.5, 10, 10);
        LUMEN_CHECK(check_viewport(in) == viewport_error::none);
    }
    // out_of_buffer: inclusive bound — x + w == W is inside, one fixed unit more is out.
    {
        auto in = buffer(100, 80);
        source(in, 50, 0, 50, 80);
        LUMEN_CHECK(check_viewport(in) == viewport_error::none);
        in.src_w += 1;
        destination(in, 10, 10);
        LUMEN_CHECK(check_viewport(in) == viewport_error::out_of_buffer);
        auto tall = buffer(100, 80);
        source(tall, 0, 1, 100, 80);
        LUMEN_CHECK(check_viewport(tall) == viewport_error::out_of_buffer);
    }
    // With scale 2 and transform 90 the bound is the turned buffer over the scale: a
    // 200x100 buffer is 50x100 in surface units.
    {
        auto in = buffer(200, 100, 2, lx::buffer_transform::rotate_90);
        source(in, 0, 0, 50, 100);
        LUMEN_CHECK(check_viewport(in) == viewport_error::none);
        source(in, 0, 0, 51, 100);
        LUMEN_CHECK(check_viewport(in) == viewport_error::out_of_buffer);
        source(in, 0, 0, 50, 101);
        LUMEN_CHECK(check_viewport(in) == viewport_error::out_of_buffer);
    }
    // No buffer: out_of_buffer cannot apply (bad_size still does).
    {
        surface_geometry_in in{};
        source(in, 1000, 1000, 10, 10);
        LUMEN_CHECK(check_viewport(in) == viewport_error::none);
        source(in, 0, 0, 10.25, 10);
        LUMEN_CHECK(check_viewport(in) == viewport_error::bad_size);
    }
}

LUMEN_TEST(buffer_scale_must_divide_the_buffer) {
    LUMEN_CHECK(check_buffer_scale(buffer(100, 100, 2)) == surface_size_error::none);
    LUMEN_CHECK(check_buffer_scale(buffer(101, 100, 2)) == surface_size_error::invalid_size);
    LUMEN_CHECK(check_buffer_scale(buffer(100, 99, 3)) == surface_size_error::invalid_size);
    // A viewport source lifts the rule (the size comes from the viewport).
    auto in = buffer(101, 100, 2);
    source(in, 0, 0, 50, 50);
    LUMEN_CHECK(check_buffer_scale(in) == surface_size_error::none);
    // No buffer, nothing to check.
    LUMEN_CHECK(check_buffer_scale(buffer(0, 0, 2)) == surface_size_error::none);
}

LUMEN_TEST(damage_converts_both_ways_for_every_transform) {
    for (unsigned t = 0; t < 8; ++t) {
        const auto tr = static_cast<lx::buffer_transform>(t);
        // Scale 2, no viewport: a surface rect maps into the buffer and back onto a rect
        // covering it.
        const auto in = buffer(200, 120, 2, tr);
        const auto geo = compute_geometry(in);
        const lx::rect2i d{5, 7, 11, 13};
        const lx::rect2i b = surface_to_buffer_damage(d, in);
        LUMEN_CHECK(b.width == 22 || b.width == 26);  // 11 or 13 surface units × 2
        LUMEN_CHECK(covers({0, 0, 200, 120}, b));
        const lx::rect2i back = buffer_to_surface_damage(b, in);
        LUMEN_CHECK(same(back, d)); // exact at an integer scale with no crop
        // The whole buffer is the whole surface, and the reverse.
        const lx::rect2i all = buffer_to_surface_damage({0, 0, 200, 120}, in);
        LUMEN_CHECK(same(all, {0, 0, geo.size.width, geo.size.height}));
        const lx::rect2i whole = surface_to_buffer_damage({0, 0, 1 << 30, 1 << 30}, in);
        LUMEN_CHECK(same(whole, {0, 0, 200, 120}));
    }
    // Normal transform at scale 1: the identity.
    {
        const auto in = buffer(100, 100);
        LUMEN_CHECK(same(surface_to_buffer_damage({3, 4, 5, 6}, in), {3, 4, 5, 6}));
        LUMEN_CHECK(same(buffer_to_surface_damage({3, 4, 5, 6}, in), {3, 4, 5, 6}));
    }
    // 90 at scale 1, 200x100 buffer (surface 100x200): surface (0,0,10,20) is the buffer's
    // bottom-left corner turned — buffer x = surface y, buffer y = 100 − surface x − w.
    {
        const auto in = buffer(200, 100, 1, lx::buffer_transform::rotate_90);
        LUMEN_CHECK(same(surface_to_buffer_damage({0, 0, 10, 20}, in), {0, 90, 20, 10}));
        LUMEN_CHECK(same(buffer_to_surface_damage({0, 90, 20, 10}, in), {0, 0, 10, 20}));
    }
    // Crop and destination scale: rounded out, never in.
    {
        auto in = buffer(400, 400);
        source(in, 100, 100, 200, 200);
        destination(in, 300, 300); // 1.5x
        // Buffer (100,100,2,2) is surface (0,0) scaled by 1.5: covers (0,0,3,3).
        const lx::rect2i s = buffer_to_surface_damage({100, 100, 2, 2}, in);
        LUMEN_CHECK(covers(s, {0, 0, 3, 3}));
        // Outside the crop: nothing.
        LUMEN_CHECK(buffer_to_surface_damage({0, 0, 50, 50}, in).width == 0);
        // Surface (0,0,3,3) back in the buffer covers (100,100,2,2).
        LUMEN_CHECK(covers(surface_to_buffer_damage({0, 0, 3, 3}, in), {100, 100, 2, 2}));
    }
}

LUMEN_TEST(projection_identity_at_scale_one) {
    const output_projection p{{100, 50}, 1.0};
    LUMEN_CHECK(same(project_node(p, {0, 0}, {120, 70}, {30, 40}), {20, 20, 30, 40}));
    LUMEN_CHECK(same(project_damage(p, {120, 70, 30, 40}), {20, 20, 30, 40}));
}

LUMEN_TEST(projection_edge_rounding) {
    // 1.25: a root at 3 lands on round(3.75) = 4; a child 2 to the right of it rounds its
    // own edges relative to the root: 4 + round(2.5) = 4 + 3 = 7, width round(12.5) − 3 = 10.
    const output_projection p{{0, 0}, 1.25};
    LUMEN_CHECK(same(project_node(p, {3, 3}, {3, 3}, {10, 10}), {4, 4, 13, 13}));
    LUMEN_CHECK(same(project_node(p, {3, 3}, {5, 3}, {8, 2}), {7, 4, 10, 3}));
    // Half away from zero, also below the origin.
    LUMEN_CHECK(project_node(p, {-14, -14}, {-14, -14}, {10, 10}).x == -18); // round(−17.5)
    // 1.5, 1.75 and 2.
    LUMEN_CHECK(same(project_node({{0, 0}, 1.5}, {1, 1}, {1, 1}, {3, 3}), {2, 2, 5, 5})); // 4.5 → 5
    LUMEN_CHECK(same(project_node({{0, 0}, 2.0}, {1, 1}, {2, 1}, {3, 3}), {4, 2, 6, 6}));
    LUMEN_CHECK(same(project_node({{0, 0}, 1.75}, {0, 0}, {0, 0}, {4, 4}), {0, 0, 7, 7}));
}

LUMEN_TEST(projection_is_pixel_exact_for_fractional_clients) {
    // A fractional-scale client renders round(w·s) pixels for a w-wide surface. Wherever the
    // root sits, its draw is exactly that wide: 1:1 texels, no resampling. And the nodes of
    // one window tile without gaps or overlaps (shared edges).
    for (const double s : {1.25, 1.5, 1.75, 2.0, 150.0 / 120.0, 175.0 / 120.0}) {
        const output_projection p{{0, 0}, s};
        unsigned failures = 0;
        for (int root = 0; root < 120; ++root) {
            for (int w = 1; w <= 2000; ++w) {
                const lx::rect2i r = project_node(p, {root, 0}, {root, 0}, {w, 1});
                if (r.width != static_cast<int>(std::lround(w * s)))
                    ++failures;
            }
            // Adjacent children of the root share edges.
            int x = root;
            int prev_end = project_node(p, {root, 0}, {x, 0}, {7, 1}).x;
            for (int k = 0; k < 50; ++k) {
                const lx::rect2i r = project_node(p, {root, 0}, {x, 0}, {7 + k % 5, 1});
                if (r.x != prev_end)
                    ++failures;
                prev_end = r.x + r.width;
                x += 7 + k % 5;
            }
        }
        if (failures)
            std::printf("    scale %.4f: %u failures\n", s, failures);
        LUMEN_CHECK(failures == 0);
    }
}

LUMEN_TEST(projected_damage_covers_the_node) {
    // Floor/ceil plus one pixel at a fractional scale: whatever the root, the node's pixels
    // lie inside its damage.
    for (const double s : {1.25, 1.5, 1.75}) {
        const output_projection p{{0, 0}, s};
        for (int root = 0; root < 60; ++root)
            for (int dx = 0; dx < 20; ++dx) {
                const lx::rect2i node = project_node(p, {root, root}, {root + dx, root}, {13, 9});
                const lx::rect2i dmg = project_damage(p, {root + dx, root, 13, 9});
                LUMEN_CHECK(covers(dmg, node));
            }
    }
    // Integer scales are not widened.
    LUMEN_CHECK(same(project_damage({{0, 0}, 2.0}, {1, 1, 3, 3}), {2, 2, 6, 6}));
    // Empty stays empty.
    LUMEN_CHECK(project_damage({{0, 0}, 1.25}, {0, 0, 0, 5}).width == 0);
}

LUMEN_TEST(scales_are_representable_in_120ths) {
    LUMEN_CHECK(representable_scale(1.25) == 150.0 / 120.0);
    LUMEN_CHECK(representable_scale(1.3333) == 160.0 / 120.0);
    LUMEN_CHECK(representable_scale(1.0) == 1.0);
    LUMEN_CHECK(representable_scale(0.0) == 1.0);
    LUMEN_CHECK(representable_scale(-2.0) == 1.0);
    LUMEN_CHECK(representable_scale(0.001) == 1.0 / 120.0);
    LUMEN_CHECK(std::lround(representable_scale(1.7) * 120.0) == 204);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
