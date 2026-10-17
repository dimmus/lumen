// The scene's output views (`lx.scene` scene_graph, ADR 008 M3): one view per output slot,
// each with its own layout rect, projection and snapshot buffer. Damage in layout
// coordinates fans out to every view it touches (projected per view), each view publishes
// on its own (one output's commit leaves the other's damage pending), a surface learns its
// visibility per view, and a view's draw list holds only what reaches its pixels.
#include "lumen_test.hpp"

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;

namespace {

// Two outputs side by side: 640x480 at scale 1, and a 400x300-logical HiDPI one (800x600
// pixels, scale 2) to its right.
lx::scene::output_view_config left_view() {
    return {{0, 0, 640, 480}, {{0, 0}, 1.0, {640, 480}}};
}
lx::scene::output_view_config right_view() {
    return {{640, 0, 400, 300}, {{640, 0}, 2.0, {800, 600}}};
}

void place(lx::scene::surface_node& n, unsigned image, lx::point2i at) {
    lx::gfx::imported_image img{};
    img.width = 100;
    img.height = 100;
    img.image_id = image;
    n.attach(img);
    n.set_bounds({0, 0, 100, 100});
    n.set_transform(at);
    n.set_opaque(true);
}

void drain(lx::scene::scene_graph& scene, unsigned slot, unsigned frame) {
    LUMEN_CHECK(scene.commit_frame(slot, frame));
    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = scene.acquire_render_snapshot(slot);
    scene.release_render_frame(slot, snap);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
}

} // namespace

LUMEN_TEST(scene_views_damage_fans_out) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    scene.set_output_view(0, left_view());
    scene.set_output_view(1, right_view());
    LUMEN_CHECK(scene.view_mask() == 0b11);
    // A new view repaints in full.
    LUMEN_CHECK(scene.pending_damage_mask() == 0b11 && scene.pending_damage(1).full);
    drain(scene, 0, 1);
    drain(scene, 1, 1);
    LUMEN_CHECK(scene.pending_damage_mask() == 0);

    // Inside the left output only.
    scene.note_damage({100, 100, 10, 10});
    LUMEN_CHECK(scene.pending_damage_mask() == 0b01);
    // Across the boundary: both, each in its own pixels.
    scene.note_damage({600, 10, 100, 50});
    LUMEN_CHECK(scene.pending_damage_mask() == 0b11);
    const lx::rect2i b = scene.pending_damage(1).bounds();
    // Logical x 640..700 is pixels 0..120 on the scale-2 output; y 10..60 is 20..120.
    LUMEN_CHECK(b.x <= 0 && b.x + b.width >= 120 && b.y <= 20 && b.y + b.height >= 120);
    LUMEN_CHECK(b.x + b.width <= 122 && b.y >= 18);
    // Below the right output (it is only 300 tall): the left one only.
    drain(scene, 0, 2);
    drain(scene, 1, 2);
    scene.note_damage({700, 400, 20, 20});
    LUMEN_CHECK(scene.pending_damage_mask() == 0);
    // A rect starting exactly at the right output's left edge touches the left one only
    // within the rounding slack: it lands on no pixel there, so the left one stays quiet.
    scene.note_damage({640, 100, 50, 50});
    LUMEN_CHECK(scene.pending_damage_mask() == 0b10);
    drain(scene, 1, 3);
    // Damage already in one output's pixels stays there.
    scene.note_damage_output(1, {0, 0, 8, 8});
    LUMEN_CHECK(scene.pending_damage_mask() == 0b10);
    scene.note_full_damage_output(0);
    LUMEN_CHECK(scene.pending_damage_mask() == 0b11 && scene.pending_damage(0).full &&
                !scene.pending_damage(1).full);
}

LUMEN_TEST(scene_views_publish_independently) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    scene.set_output_view(0, left_view());
    scene.set_output_view(1, right_view());
    drain(scene, 0, 1);
    drain(scene, 1, 1);
    scene.note_damage({600, 10, 100, 50});
    // Only the right output's clock is due: the left one keeps its damage for its own tick.
    drain(scene, 1, 2);
    LUMEN_CHECK(scene.pending_damage_mask() == 0b01);
    drain(scene, 0, 3);
    LUMEN_CHECK(scene.pending_damage_mask() == 0);
}

LUMEN_TEST(scene_views_visibility_per_output) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    scene.set_output_view(0, left_view());
    scene.set_output_view(1, right_view());
    lx::scene::surface_node left{lx::surface_id{1}};
    lx::scene::surface_node across{lx::surface_id{2}};
    lx::scene::surface_node right{lx::surface_id{3}};
    place(left, 11, {100, 100});
    place(across, 12, {600, 20});
    place(right, 13, {800, 50});
    LUMEN_CHECK(scene.root().add(&left) && scene.root().add(&across) && scene.root().add(&right));

    LUMEN_CHECK(scene.commit_frame(0, 4));
    LUMEN_CHECK(scene.commit_frame(1, 9));
    LUMEN_CHECK(left.visible_in(4, 0) && !left.visible_in(9, 1));
    LUMEN_CHECK(across.visible_in(4, 0) && across.visible_in(9, 1));
    LUMEN_CHECK(!right.visible_in(4, 0) && right.visible_in(9, 1));
    // Frame numbers are per view: frame 4 of view 1 was never published.
    LUMEN_CHECK(!across.visible_in(4, 1) && across.shown_frame(1) == 9);

    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& s0 = scene.acquire_render_snapshot(0);
    const auto& s1 = scene.acquire_render_snapshot(1);
    // Each view's list holds what reaches its pixels only.
    LUMEN_CHECK(s0.frame_index() == 4 && s0.draws().size() == 2);
    LUMEN_CHECK(s1.frame_index() == 9 && s1.draws().size() == 2);
    // The right view's projection: logical (800, 50) is pixel (320, 100), 200 px wide.
    bool found = false;
    for (unsigned i = 0; i < s1.draws().size(); ++i) {
        const auto& d = s1.draws().data()[i];
        if (d.dst.x == 320 && d.dst.y == 100 && d.dst.width == 200 && d.dst.height == 200)
            found = true;
    }
    LUMEN_CHECK(found);
    scene.release_render_frame(0, s0);
    scene.release_render_frame(1, s1);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    (void)scene.root().remove(&left);
    (void)scene.root().remove(&across);
    (void)scene.root().remove(&right);
}

LUMEN_TEST(scene_views_remove_and_reconfigure) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    scene.set_output_view(0, left_view());
    scene.set_output_view(1, right_view());
    drain(scene, 0, 1);
    drain(scene, 1, 1);
    // An unchanged configuration is not a repaint.
    scene.set_output_view(1, right_view());
    LUMEN_CHECK(scene.pending_damage_mask() == 0);
    // A moved output repaints in full.
    auto moved = right_view();
    moved.logical.x = 0;
    moved.logical.y = 480;
    moved.projection.origin = {0, 480};
    scene.set_output_view(1, moved);
    LUMEN_CHECK(scene.pending_damage_mask() == 0b10 && scene.pending_damage(1).full);
    drain(scene, 1, 2);
    // A removed view takes no damage and publishes nothing.
    scene.remove_output_view(1);
    LUMEN_CHECK(scene.view_mask() == 0b01 && !scene.has_view(1));
    scene.note_damage({0, 0, 2000, 2000});
    LUMEN_CHECK(scene.pending_damage_mask() == 0b01);
    LUMEN_CHECK(!scene.commit_frame(1, 3));
}

// ── Output transforms (ADR 008 §C.4) ──

namespace {

struct pt {
    float x, y;
};

pt apply_basis(lx::buffer_transform t, pt q) {
    const lx::gfx::source_basis b = lx::gfx::source_basis_for(t);
    return {b.origin[0] + q.x * b.dx[0] + q.y * b.dy[0], b.origin[1] + q.x * b.dx[1] + q.y * b.dy[1]};
}

/// The inverse of `apply_basis`: the matrix is orthogonal.
pt invert_basis(lx::buffer_transform t, pt p) {
    const lx::gfx::source_basis b = lx::gfx::source_basis_for(t);
    const float rx = p.x - b.origin[0];
    const float ry = p.y - b.origin[1];
    return {rx * b.dx[0] + ry * b.dx[1], rx * b.dy[0] + ry * b.dy[1]};
}

} // namespace

LUMEN_TEST(output_transform_composition_samples_the_right_texel) {
    // For every buffer transform B and output transform T: the output pixel p shows content
    // R_T⁻¹(p), which is buffer texel R_B(R_T⁻¹(p)); the composed draw transform must sample
    // exactly that.
    const pt samples[] = {{0.1f, 0.2f}, {0.75f, 0.3f}, {0.4f, 0.9f}, {0.f, 1.f}};
    for (unsigned b = 0; b < 8; ++b)
        for (unsigned t = 0; t < 8; ++t) {
            const auto bt = static_cast<lx::buffer_transform>(b);
            const auto ot = static_cast<lx::buffer_transform>(t);
            const auto x = lx::gfx::compose_output_transform(bt, ot);
            for (const pt& p : samples) {
                const pt want = apply_basis(bt, invert_basis(ot, p));
                const pt got = apply_basis(x, p);
                const float ex = want.x - got.x;
                const float ey = want.y - got.y;
                LUMEN_CHECK(ex < 1e-5f && ex > -1e-5f && ey < 1e-5f && ey > -1e-5f);
            }
        }
    // The identity cases.
    LUMEN_CHECK(lx::gfx::compose_output_transform(lx::buffer_transform::rotate_90,
                                                  lx::buffer_transform::normal) ==
                lx::buffer_transform::rotate_90);
    // A buffer rendered with the output's own transform is shown as it is (the point of
    // wl_output.transform: direct scanout).
    for (unsigned t = 0; t < 8; ++t)
        LUMEN_CHECK(lx::gfx::compose_output_transform(static_cast<lx::buffer_transform>(t),
                                                      static_cast<lx::buffer_transform>(t)) ==
                    lx::buffer_transform::normal);
}

LUMEN_TEST(output_transform_rects_are_exact) {
    // 90°: content (x, y) → output (y, W − x) — counter-clockwise, as wl_output.transform says.
    const lx::rect2i r = lx::gfx::transform_rect({10, 20, 30, 40}, lx::buffer_transform::rotate_90,
                                                 480, 640);
    LUMEN_CHECK(r.x == 20 && r.y == 480 - 10 - 30 && r.width == 40 && r.height == 30);
    for (unsigned t = 0; t < 8; ++t) {
        const auto tt = static_cast<lx::buffer_transform>(t);
        const bool turned = (t & 1u) != 0;
        // The whole content space is the whole output.
        const lx::rect2i all = lx::gfx::transform_rect({0, 0, 480, 640}, tt, 480, 640);
        LUMEN_CHECK(all.x == 0 && all.y == 0 && all.width == (turned ? 640 : 480) &&
                    all.height == (turned ? 480 : 640));
        // Every pixel lands on exactly one pixel, inside.
        const lx::rect2i px = lx::gfx::transform_rect({479, 0, 1, 1}, tt, 480, 640);
        LUMEN_CHECK(px.width == 1 && px.height == 1 && px.x >= 0 && px.y >= 0 &&
                    px.x < all.width && px.y < all.height);
        // The rect map agrees with the basis map (centres of pixels).
        const pt c = apply_basis(tt, {(10.f + 15.f) / 480.f, (20.f + 20.f) / 640.f});
        const lx::rect2i m = lx::gfx::transform_rect({10, 20, 30, 40}, tt, 480, 640);
        const float cx = (m.x + m.width / 2.f) / all.width;
        const float cy = (m.y + m.height / 2.f) / all.height;
        LUMEN_CHECK(c.x - cx < 1e-4f && cx - c.x < 1e-4f && c.y - cy < 1e-4f && cy - c.y < 1e-4f);
    }
}

LUMEN_TEST(scene_view_with_a_turned_output) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    // A 640x480 monitor turned 90°: 480x640 logical.
    lx::scene::output_view_config v{};
    v.logical = {0, 0, 480, 640};
    v.projection = {{0, 0}, 1.0, {640, 480}, lx::buffer_transform::rotate_90};
    scene.set_output_view(0, v);
    lx::scene::surface_node n{lx::surface_id{7}};
    place(n, 21, {10, 20});
    LUMEN_CHECK(scene.root().add(&n));
    drain(scene, 0, 1);
    // Damage turns with the output.
    scene.note_damage({10, 20, 100, 100});
    const lx::rect2i d = scene.pending_damage(0).bounds();
    LUMEN_CHECK(d.x == 20 && d.y == 480 - 10 - 100 && d.width == 100 && d.height == 100);
    LUMEN_CHECK(scene.commit_frame(0, 2));
    LUMEN_CHECK(n.visible_in(2, 0));
    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& s = scene.acquire_render_snapshot(0);
    bool found = false;
    for (unsigned i = 0; i < s.draws().size(); ++i) {
        const auto& dc = s.draws().data()[i];
        if (dc.dst.x == 20 && dc.dst.y == 370 && dc.dst.width == 100 && dc.dst.height == 100 &&
            dc.buffer_xform == lx::gfx::compose_output_transform(lx::buffer_transform::normal,
                                                                 lx::buffer_transform::rotate_90))
            found = true;
    }
    LUMEN_CHECK(found);
    scene.release_render_frame(0, s);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    // A surface past the content's right edge (logical x ≥ 480) is off the output.
    n.set_transform({500, 20});
    LUMEN_CHECK(scene.commit_frame(0, 3));
    LUMEN_CHECK(!n.visible_in(3, 0));
    (void)scene.root().remove(&n);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
