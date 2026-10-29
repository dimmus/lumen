// Window animations (config.toml [animations], `lx.compositor:window_animation`), every frame
// time injected so each pose is checked against the spring's closed form computed here:
//
// - the spring: the critically damped closed form, no overshoot, convergence, an underdamped
//   spring bounces and its reach bounds every later excursion, an overdamped one creeps; a
//   value stops — landing on its target — at exactly the first frame whose reach is under
//   half a pixel, and a retarget is continuous (one that had settled starts again with the
//   next frame, from where it was moved to);
// - the scene: an animated window's draws are mapped and faded (and drawn exactly as before
//   once the animation is gone), a confined slide draws nothing or the window at rest on the
//   other output, capture views draw the end state (at rest, or nothing of a leaving window),
//   primitives scale, fade and take their orders from where they now are;
// - the buffer lifecycle: a closing window's ghost keeps its dma-buf current until it lets go,
//   a buffer the surface shows again is not released by the hold ending, and a newer ghost's
//   hold outlives the older one's;
// - in-process against a real compositor (one virtual 1280x720 output, no shell): off by
//   default nothing animates; on, a window opens, closes (a ghost), slides with a workspace
//   switch, minimizes and comes back, maximizes and restores in the maximize's tail
//   visually — every intermediate frame's
//   draw equal to the closed form — the client configured for its final size at once, and
//   the desktop goes quiet (no damage, nothing animating) once the motion settles.
#include "lumen_test.hpp"

#include <errno.h>
#include <stdio.h>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#define LUMEN_TEST_WM_ANIM 1
#endif
#endif
#include <stdint.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.scheduler;
import lx.compositor;

namespace {

constexpr double k_stiffness = 800.0;
const double k_w0 = std::sqrt(k_stiffness);

/// Critically damped from `x0` at rest: x0·(1 + ω₀t)·e^(−ω₀t).
double crit_offset(double x0, double t) { return x0 * (1.0 + k_w0 * t) * std::exp(-k_w0 * t); }
/// Its reach √(x² + v²/ω₀²) = |x0|·e^(−ω₀t)·√((1 + ω₀t)² + (ω₀t)²).
double crit_reach(double x0, double t) {
    return std::abs(x0) * std::exp(-k_w0 * t) * std::hypot(1.0 + k_w0 * t, k_w0 * t);
}

constexpr long long k_dt = 16'666'667; // ns, 60 Hz

} // namespace

// ── the spring ───────────────────────────────────────────────────────────────

LUMEN_TEST(spring_critically_damped_matches_the_closed_form) {
    double worst = 0.0;
    for (int ms = 0; ms <= 1000; ++ms) {
        const double t = ms * 1e-3;
        const auto s = lx::compositor::spring_step({100.0, 0.0}, t, k_stiffness, 1.0);
        worst = std::max(worst, std::abs(s.offset - crit_offset(100.0, t)));
        // v = −x0·ω₀²·t·e^(−ω₀t)
        const double v = -100.0 * k_w0 * k_w0 * t * std::exp(-k_w0 * t);
        worst = std::max(worst, std::abs(s.velocity - v) / k_w0);
    }
    std::printf("    critical: worst |x - x0(1+wt)e^-wt| %.3g\n", worst);
    LUMEN_CHECK(worst < 1e-9);
}

LUMEN_TEST(spring_critically_damped_never_overshoots_and_converges) {
    double previous = 100.0;
    bool ok = true;
    for (int ms = 1; ms <= 3000; ++ms) {
        const auto s = lx::compositor::spring_step({100.0, 0.0}, ms * 1e-3, k_stiffness, 1.0);
        ok = ok && s.offset >= 0.0 && s.offset <= previous && s.velocity <= 0.0;
        previous = s.offset;
    }
    LUMEN_CHECK(ok);
    LUMEN_CHECK(previous < 1e-6);
    // Released toward its target at speed, a critical spring still arrives without crossing
    // when the speed is below ω₀·x0.
    const auto fast = lx::compositor::spring_step({100.0, -0.5 * 100.0 * k_w0}, 0.0, k_stiffness, 1.0);
    LUMEN_CHECK(fast.offset == 100.0);
    for (int ms = 1; ms <= 3000; ++ms)
        ok = ok && lx::compositor::spring_step({100.0, -0.5 * 100.0 * k_w0}, ms * 1e-3,
                                               k_stiffness, 1.0)
                           .offset >= 0.0;
    LUMEN_CHECK(ok);
}

LUMEN_TEST(spring_underdamped_bounces_and_its_reach_bounds_what_follows) {
    constexpr double zeta = 0.5;
    const double wd = k_w0 * std::sqrt(1.0 - zeta * zeta);
    bool crossed = false;
    double worst = 0.0;
    std::vector<lx::compositor::spring_state> states;
    for (int ms = 0; ms <= 1500; ++ms) {
        const double t = ms * 1e-3;
        const auto s = lx::compositor::spring_step({100.0, 0.0}, t, k_stiffness, zeta);
        // x0·e^(−ζω₀t)·(cos ω_d t + ζω₀/ω_d · sin ω_d t)
        const double x = 100.0 * std::exp(-zeta * k_w0 * t) *
                         (std::cos(wd * t) + zeta * k_w0 / wd * std::sin(wd * t));
        worst = std::max(worst, std::abs(s.offset - x));
        crossed = crossed || s.offset < 0.0;
        states.push_back(s);
    }
    LUMEN_CHECK(worst < 1e-9);
    LUMEN_CHECK(crossed);
    bool bounded = true;
    for (std::size_t i = 0; i < states.size(); i += 7) {
        const double reach = lx::compositor::spring_reach(states[i], k_stiffness);
        for (std::size_t j = i; j < states.size(); ++j)
            bounded = bounded && std::abs(states[j].offset) <= reach + 1e-9;
    }
    LUMEN_CHECK(bounded);
}

LUMEN_TEST(spring_overdamped_creeps_without_overshoot) {
    bool ok = true;
    for (int ms = 1; ms <= 2000; ++ms) {
        const auto o = lx::compositor::spring_step({100.0, 0.0}, ms * 1e-3, k_stiffness, 1.5);
        ok = ok && o.offset >= 0.0;
    }
    LUMEN_CHECK(ok);
    const double over = lx::compositor::spring_step({100.0, 0.0}, 0.2, k_stiffness, 1.5).offset;
    LUMEN_CHECK(over > crit_offset(100.0, 0.2));
}

LUMEN_TEST(spring_value_stops_at_the_first_frame_under_half_a_pixel) {
    lx::compositor::animation_config c{true, k_stiffness, 1.0};
    lx::compositor::spring_value v;
    v.launch(200.0, 0.0);
    const long long t0 = 5'000'000'000;
    int expected_stop = -1;
    for (int n = 0; n < 200 && expected_stop < 0; ++n)
        if (crit_reach(200.0, n * k_dt * 1e-9) < 0.5)
            expected_stop = n;
    LUMEN_CHECK(expected_stop > 0);
    int stopped = -1;
    double worst = 0.0;
    for (int n = 0; n <= expected_stop + 2; ++n) {
        const bool moving = v.sample(t0 + n * k_dt, c, 0.5);
        if (!moving && stopped < 0)
            stopped = n;
        if (moving)
            worst = std::max(worst, std::abs(v.value - crit_offset(200.0, n * k_dt * 1e-9)));
    }
    std::printf("    stops at frame %d (%.1f ms), expected %d; worst value error %.3g px\n",
                stopped, stopped * k_dt * 1e-6, expected_stop, worst);
    LUMEN_CHECK(stopped == expected_stop);
    LUMEN_CHECK(v.value == 0.0 && v.velocity == 0.0 && !v.moving);
    LUMEN_CHECK(worst < 1e-9);
    // The frame before still had more than half a pixel to go.
    LUMEN_CHECK(crit_reach(200.0, (expected_stop - 1) * k_dt * 1e-9) >= 0.5);
}

LUMEN_TEST(spring_value_retarget_is_continuous) {
    lx::compositor::animation_config c{true, k_stiffness, 1.0};
    lx::compositor::spring_value v;
    v.launch(100.0, 0.0);
    const long long t0 = 1'000'000'000;
    (void)v.sample(t0, c, 0.5);
    (void)v.sample(t0 + 3 * k_dt, c, 0.5);
    const double x = v.value;
    const double vel = v.velocity;
    v.retarget(50.0);
    LUMEN_CHECK(v.value == x && v.velocity == vel && v.moving);
    (void)v.sample(t0 + 4 * k_dt, c, 0.5);
    const auto s = lx::compositor::spring_step({x - 50.0, vel}, k_dt * 1e-9, k_stiffness, 1.0);
    LUMEN_CHECK(std::abs(v.value - (50.0 + s.offset)) < 1e-9);
}

LUMEN_TEST(spring_value_retarget_after_settling_starts_with_the_next_frame) {
    lx::compositor::animation_config c{true, k_stiffness, 1.0};
    lx::compositor::spring_value v;
    v.launch(-100.0, 0.0);
    const long long t0 = 1'000'000'000;
    int n = 0;
    while (v.sample(t0 + n * k_dt, c, 0.5) && n < 100)
        ++n;
    LUMEN_CHECK(!v.moving && v.value == 0.0);
    // Three frames later, what it is measured from moves 100 px under it: the visual stays
    // put for the first frame, then the closed form from 100 at rest.
    v.shift(100.0);
    const long long ts = t0 + (n + 3) * k_dt;
    double worst = 0.0;
    for (int k = 0; k < 5; ++k) {
        LUMEN_CHECK(v.sample(ts + k * k_dt, c, 0.5));
        worst = std::max(worst, std::abs(v.value - crit_offset(100.0, k * k_dt * 1e-9)));
    }
    std::printf("    settled at frame %d; after the shift worst value error %.3g px\n", n, worst);
    LUMEN_CHECK(worst < 1e-9);
}

// ── the scene ────────────────────────────────────────────────────────────────

namespace {

struct one_window {
    lx::scene::scene_graph scene;
    lx::scene::container_node box;
    lx::scene::surface_node node{lx::surface_id{1}};
    one_window(lx::point2i at, lx::size2i size) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        box.set_anchor(true);
        box.set_transform(at);
        lx::gfx::imported_image img{};
        img.image_id = 7;
        img.width = static_cast<unsigned>(size.width);
        img.height = static_cast<unsigned>(size.height);
        node.attach(img);
        node.set_bounds({0, 0, size.width, size.height});
        node.set_transform(at);
        node.set_opaque(true);
        (void)box.add(&node);
        (void)scene.root().add(&box);
    }
    ~one_window() {
        (void)box.remove(&node);
        (void)scene.root().remove(&box);
    }
};

} // namespace

LUMEN_TEST(scene_maps_and_fades_an_animated_window_and_restores_it_exactly) {
    one_window w{{100, 50}, {200, 100}};
    lx::scene::draw_list rest;
    w.scene.build_draw_list_into(rest);
    LUMEN_CHECK(rest.size() == 1);
    const lx::scene::draw_command plain = rest.data()[0];
    LUMEN_CHECK(plain.dst.x == 100 && plain.dst.width == 200 && plain.blend == lx::blend_mode::opaque);

    lx::scene::node_animation a{};
    a.active = true;
    a.from_x = 100;
    a.from_y = 50;
    a.to_x = 110;
    a.to_y = 55;
    a.scale_x = 0.9;
    a.scale_y = 0.9;
    a.opacity = 0.5f;
    w.box.set_animation(a);
    lx::scene::draw_list moved;
    w.scene.build_draw_list_into(moved);
    LUMEN_CHECK(moved.size() == 1);
    const lx::scene::draw_command& d = moved.data()[0];
    // x' = 110 + (x − 100)·0.9: [100, 300) → [110, 290), [50, 150) → [55, 145).
    LUMEN_CHECK(d.dst.x == 110 && d.dst.y == 55 && d.dst.width == 180 && d.dst.height == 90);
    LUMEN_CHECK(d.opacity == 0.5f && d.blend == lx::blend_mode::premultiplied && d.scaled);
    LUMEN_CHECK(d.transform.tx == 110.f && d.transform.ty == 55.f);

    // A translation only: still 1:1, still opaque at full opacity.
    a.to_x = 103.4;
    a.scale_x = a.scale_y = 1.0;
    a.opacity = 1.f;
    w.box.set_animation(a);
    moved.clear();
    w.scene.build_draw_list_into(moved);
    LUMEN_CHECK(moved.data()[0].dst.x == 103 && moved.data()[0].dst.width == 200 &&
                !moved.data()[0].scaled && moved.data()[0].blend == lx::blend_mode::opaque);

    // Gone: exactly the draw it had.
    w.box.set_animation({});
    lx::scene::draw_list again;
    w.scene.build_draw_list_into(again);
    const lx::scene::draw_command& e = again.data()[0];
    LUMEN_CHECK(std::memcmp(&e.dst, &plain.dst, sizeof(e.dst)) == 0 && e.opacity == plain.opacity &&
                e.blend == plain.blend && e.scaled == plain.scaled &&
                e.transform.tx == plain.transform.tx && e.transform.ty == plain.transform.ty);
}

LUMEN_TEST(scene_confines_a_slide_to_its_output) {
    one_window w{{700, 100}, {200, 100}};
    lx::scene::output_view_config left{};
    left.logical = {0, 0, 800, 600};
    left.projection.size = {800, 600};
    lx::scene::output_view_config right{};
    right.logical = {800, 0, 800, 600};
    right.projection.origin = {800, 0};
    right.projection.size = {800, 600};
    w.scene.set_output_view(0, left);
    w.scene.set_output_view(1, right);
    lx::scene::node_animation a{};
    a.active = true;
    a.from_x = 700;
    a.from_y = 100;
    a.to_x = 400;
    a.to_y = 100;
    a.view = 0;
    a.elsewhere_hidden = true;
    w.box.set_animation(a);
    lx::scene::draw_list l;
    lx::scene::draw_list r;
    w.scene.build_draw_list_into(l, 0);
    w.scene.build_draw_list_into(r, 1);
    LUMEN_CHECK(l.size() == 1 && l.data()[0].dst.x == 400);
    LUMEN_CHECK(r.size() == 0); // leaving that workspace: nothing of it on the other output
    a.elsewhere_hidden = false;
    w.box.set_animation(a);
    r.clear();
    w.scene.build_draw_list_into(r, 1);
    // At rest there: its right part, x 700 - 800 = -100.
    LUMEN_CHECK(r.size() == 1 && r.data()[0].dst.x == -100);
}

LUMEN_TEST(scene_capture_views_draw_where_a_motion_ends) {
    one_window w{{100, 50}, {200, 100}};
    lx::scene::output_view_config out{};
    out.logical = {0, 0, 800, 600};
    out.projection.size = {800, 600};
    w.scene.set_output_view(0, out);
    const unsigned cap = lx::scene::k_capture_view_base;
    w.scene.set_output_view(cap, out);
    lx::scene::node_animation a{};
    a.active = true;
    a.from_x = 100;
    a.from_y = 50;
    a.to_x = 110;
    a.to_y = 55;
    a.scale_x = a.scale_y = 0.9;
    a.opacity = 0.5f;
    w.box.set_animation(a);
    lx::scene::draw_list o;
    lx::scene::draw_list c;
    w.scene.build_draw_list_into(o, 0);
    w.scene.build_draw_list_into(c, cap);
    // Arriving (an open): the output shows the motion, a capture the window at rest.
    LUMEN_CHECK(o.size() == 1 && o.data()[0].dst.x == 110 && o.data()[0].opacity == 0.5f);
    LUMEN_CHECK(c.size() == 1 && c.data()[0].dst.x == 100 && c.data()[0].dst.width == 200 &&
                c.data()[0].opacity == 1.f && !c.data()[0].scaled);
    // Leaving (a ghost, a minimize, a slide off its workspace): nothing of it in a capture.
    a.leaving = true;
    a.view = 0;
    w.box.set_animation(a);
    o.clear();
    c.clear();
    w.scene.build_draw_list_into(o, 0);
    w.scene.build_draw_list_into(c, cap);
    LUMEN_CHECK(o.size() == 1 && o.data()[0].dst.x == 110);
    LUMEN_CHECK(c.size() == 0);
    w.scene.remove_output_view(cap);
}

namespace {

/// One solid quad at a logical rect (a server-side frame's fill, say).
class quad_node final : public lx::scene::scene_node {
public:
    explicit quad_node(lx::rect2i r) : rect_{r} {}
    [[nodiscard]] lx::scene::node_kind kind() const override { return lx::scene::node_kind::widget; }
    using lx::scene::scene_node::emit_draws;
    void emit_draws(lx::scene::draw_list& out, const lx::scene::emit_context&) const override {
        lx::gfx::quad_instance q{};
        q.bounds[0] = static_cast<float>(rect_.x);
        q.bounds[1] = static_cast<float>(rect_.y);
        q.bounds[2] = static_cast<float>(rect_.width);
        q.bounds[3] = static_cast<float>(rect_.height);
        q.radii[0] = lx::gfx::to_half(4.f);
        q.border_widths[0] = lx::gfx::to_half(1.f);
        const lx::gfx::packed_rgba c = lx::gfx::premultiplied_half(lx::color::rgb(1.f, 0.f, 0.f, 1.f));
        q.background[0] = c.r;
        q.background[1] = c.g;
        q.background[2] = c.b;
        q.background[3] = c.a;
        for (unsigned k = 0; k < 4; ++k)
            q.background_end[k] = q.background[k];
        out.push_quad(q, rect_, out.orders().assign(rect_));
    }

private:
    lx::rect2i rect_{};
};

} // namespace

LUMEN_TEST(scene_scales_fades_and_reorders_animated_primitives) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene;
    lx::scene::container_node box;
    box.set_anchor(true);
    quad_node a{{10, 10, 20, 20}};
    quad_node b{{100, 100, 10, 10}}; // another window, above: overlaps only where `a` grows to
    (void)box.add(&a);
    (void)scene.root().add(&box);
    (void)scene.root().add(&b);
    lx::scene::node_animation an{};
    an.active = true;
    an.scale_x = an.scale_y = 4.0; // about (0, 0): [10, 30) → [40, 120)
    an.opacity = 0.5f;
    box.set_animation(an);
    lx::scene::draw_list list;
    scene.build_draw_list_into(list);
    const auto quads = list.quad_instances();
    const auto refs = list.pushed_primitives();
    LUMEN_CHECK(quads.size() == 2 && refs.size() == 2);
    const lx::gfx::quad_instance& q = quads[0];
    LUMEN_CHECK(q.bounds[0] == 40.f && q.bounds[1] == 40.f && q.bounds[2] == 80.f && q.bounds[3] == 80.f);
    LUMEN_CHECK(lx::gfx::from_half(q.radii[0]) == 16.f && lx::gfx::from_half(q.border_widths[0]) == 4.f);
    LUMEN_CHECK(lx::gfx::from_half(q.background[3]) == 0.5f && lx::gfx::from_half(q.background[0]) == 0.5f);
    // Where `a` now is, `b` (pushed after it) is painted over it.
    std::printf("    orders: animated %u, the window above %u\n", refs[0].order, refs[1].order);
    LUMEN_CHECK(refs[1].order > refs[0].order);
    (void)scene.root().remove(&b);
    (void)box.remove(&a);
    (void)scene.root().remove(&box);
}

// ── the buffer lifecycle ─────────────────────────────────────────────────────

namespace {

struct lifecycle_rig {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::dmabuf_import_cache cache{importer, {}};
    lx::compositor::buffer_lifecycle_tracker tracker{cache};
    std::vector<void*> released;
    lifecycle_rig() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        tracker.set_release_callback(
            [](void* b, void* u) { static_cast<lifecycle_rig*>(u)->released.push_back(b); },
            this);
    }
    void commit(unsigned surface, unsigned buffer, void* resource) {
        lx::gfx::import_cache_key key{};
        key.client = lx::client_id{1};
        key.buffer = lx::buffer_id{buffer};
        LUMEN_CHECK(static_cast<bool>(tracker.on_import(lx::client_id{1}, lx::surface_id{surface},
                                                        lx::buffer_id{buffer}, key, resource)));
        tracker.on_attach(lx::surface_id{surface});
    }
};

int g_buf_a = 0;
int g_buf_b = 0;

} // namespace

LUMEN_TEST(lifecycle_keeps_a_ghosts_dmabuf_until_it_lets_go) {
    // Without a hold: the surface goes, the buffer is released once its frames completed.
    {
        lifecycle_rig r;
        r.commit(1, 10, &g_buf_a);
        r.tracker.on_frame_submitted(5);
        r.tracker.on_detach(lx::surface_id{1});
        (void)r.tracker.retire_completed_frames(5);
        LUMEN_CHECK(r.released.size() == 1 && r.released[0] == &g_buf_a);
    }
    // A ghost holds it after the surface went (the wire detaches before the window unmaps).
    lifecycle_rig r;
    r.commit(1, 10, &g_buf_a);
    r.tracker.on_frame_submitted(5);
    r.tracker.on_detach(lx::surface_id{1});
    r.tracker.hold(lx::surface_id{1});
    (void)r.tracker.retire_completed_frames(100);
    LUMEN_CHECK(r.released.empty());
    LUMEN_CHECK(!r.tracker.has_unreleased_frames());
    r.tracker.on_frame_submitted(101); // the ghost's frames sample it
    // The surface showing something new meanwhile does not supersede what the ghost draws.
    r.commit(1, 11, &g_buf_b);
    r.tracker.on_frame_submitted(102);
    (void)r.tracker.retire_completed_frames(200);
    LUMEN_CHECK(r.released.empty());
    r.tracker.on_frame_submitted(201);
    r.tracker.release_hold(lx::surface_id{1});
    LUMEN_CHECK(r.tracker.has_unreleased_frames());
    (void)r.tracker.retire_completed_frames(200);
    LUMEN_CHECK(r.released.empty()); // frame 201 sampled it and has not completed
    (void)r.tracker.retire_completed_frames(201);
    LUMEN_CHECK(r.released.size() == 1 && r.released[0] == &g_buf_a);

    // The surface shows the held buffer again (it remapped with the same wl_buffer): the
    // ghost letting go does not release what is on screen.
    {
        lifecycle_rig q;
        q.commit(1, 10, &g_buf_a);
        q.tracker.on_frame_submitted(5);
        q.tracker.on_detach(lx::surface_id{1});
        q.tracker.hold(lx::surface_id{1});
        q.commit(1, 10, &g_buf_a);
        q.tracker.on_frame_submitted(6);
        q.tracker.release_hold(lx::surface_id{1});
        (void)q.tracker.retire_completed_frames(100);
        LUMEN_CHECK(q.released.empty());
        // Superseded by the surface itself, it goes as usual.
        q.commit(1, 11, &g_buf_b);
        (void)q.tracker.retire_completed_frames(101);
        LUMEN_CHECK(q.released.size() == 1 && q.released[0] == &g_buf_a);
    }
    // An older ghost of the surface (it remapped unanimated and unmaps again): the window
    // manager ends it before the new ghost holds, so the new hold is the newer ghost's alone.
    {
        lifecycle_rig q;
        q.commit(1, 10, &g_buf_a);
        q.tracker.on_frame_submitted(5);
        q.tracker.on_detach(lx::surface_id{1});
        q.tracker.hold(lx::surface_id{1}); // ghost 1: a
        q.commit(1, 11, &g_buf_b);          // remapped
        q.tracker.on_frame_submitted(6);
        q.tracker.on_detach(lx::surface_id{1});
        q.tracker.release_hold(lx::surface_id{1}); // ghost 1 ends first
        q.tracker.hold(lx::surface_id{1});         // ghost 2: b
        (void)q.tracker.retire_completed_frames(100);
        LUMEN_CHECK(q.released.size() == 1 && q.released[0] == &g_buf_a);
        q.tracker.on_frame_submitted(101);
        q.tracker.release_hold(lx::surface_id{1});
        (void)q.tracker.retire_completed_frames(101);
        LUMEN_CHECK(q.released.size() == 2 && q.released[1] == &g_buf_b);
    }
}

// ── in-process: a real compositor ────────────────────────────────────────────

#if !defined(LUMEN_TEST_WM_ANIM)

int run_wm_cases() {
    std::printf("SKIP: window-manager cases need libwayland and the xdg-shell glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-window-animations";

int g_failed = 0;
int g_ran = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 2'000'000;
long long g_now = 10'000'000'000;

void check(const char* name, bool ok, const std::string& detail = {}) {
    ++g_ran;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " — ",
                detail.c_str());
    if (!ok)
        ++g_failed;
}

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
};

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        c->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(r, name, &xdg_wm_base_interface, version < 6 ? version : 6));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    uint32_t serial = 0;
    uint32_t acked = 0;
    int width = 0;
    int height = 0;
};

const xdg_surface_listener xdg_listener = {
    [](void* d, xdg_surface*, uint32_t serial) { static_cast<window*>(d)->serial = serial; }};
const xdg_toplevel_listener toplevel_listener = {
    [](void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
        auto* win = static_cast<window*>(d);
        win->width = w;
        win->height = h;
    },
    [](void*, xdg_toplevel*) {},
    [](void*, xdg_toplevel*, int32_t, int32_t) {},
    [](void*, xdg_toplevel*, wl_array*) {}};

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-test-window-animations", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/// One frame tick at the injected time `g_now` (then advanced by a 60 Hz period).
void tick() {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::scheduler::frame_tick t{};
    t.timestamp_ns = g_now;
    t.delta_seconds = 1.0 / 60.0;
    t.frame_index = static_cast<unsigned>(++g_frame);
    g_comp->tick_ui(t);
    g_comp->wayland().flush();
    g_now += k_dt;
}

/// Acks the newest configure and commits a buffer of its size (200x150 when the client
/// picks).
void answer(client& c, window& w) {
    xdg_surface_ack_configure(w.xdg, w.serial);
    w.acked = w.serial;
    const int bw = w.width > 0 ? w.width : 200;
    const int bh = w.height > 0 ? w.height : 150;
    wl_surface_attach(w.surface, make_buffer(c, bw, bh), 0, 0);
    wl_surface_commit(w.surface);
}

/// Maps a window without ticking: its first frame is the next `tick`.
void map_window(client& c, window& w, const char* app_id) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_add_listener(w.toplevel, &toplevel_listener, &w);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    answer(c, w);
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
}

void destroy_window(client& c, window& w) {
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    w = {};
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
}

struct rec_copy {
    lx::toplevel_id id{};
    lx::rect2i geometry{};
    bool mapped = false;
};

rec_copy record(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    rec_copy out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id.id())
            out = {r.id, r.geometry, r.mapped};
    });
    return out;
}

template <typename Fn>
auto with_wm(Fn&& fn) {
    std::lock_guard<std::mutex> hold(*g_lock);
    struct flush_after {
        ~flush_after() { g_comp->wayland().flush(); }
    } f;
    return fn(g_comp->windows());
}

/// The one client draw on output 0 (a textured draw at least 50 px wide), as the next
/// published snapshot will have it; `found` false when there is none.
struct seen {
    bool found = false;
    lx::rect2i dst{};
    float opacity = 0.f;
    bool scaled = false;
};
seen window_draw() {
    std::lock_guard<std::mutex> hold(*g_lock);
    static lx::scene::draw_list list;
    g_comp->scene().build_draw_list_into(list, 0);
    seen s{};
    for (unsigned i = 0; i < list.size(); ++i) {
        const lx::scene::draw_command& d = list.data()[i];
        if (d.solid || !d.texture || d.dst.width < 50)
            continue;
        s = {true, d.dst, d.opacity, d.scaled};
    }
    return s;
}

/// The same, as a capture view of output 0 (a screenshot, a screencast) would draw it now.
seen capture_draw() {
    std::lock_guard<std::mutex> hold(*g_lock);
    static lx::scene::draw_list list;
    lx::scene::scene_graph& scene = g_comp->scene();
    const unsigned cap = lx::scene::k_capture_view_base;
    scene.set_output_view(cap, scene.view_config(0));
    scene.build_draw_list_into(list, cap);
    scene.remove_output_view(cap);
    seen s{};
    for (unsigned i = 0; i < list.size(); ++i) {
        const lx::scene::draw_command& d = list.data()[i];
        if (d.solid || !d.texture || d.dst.width < 50)
            continue;
        s = {true, d.dst, d.opacity, d.scaled};
    }
    return s;
}

bool animating() {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->windows().animations().active();
}

std::uint32_t pending_damage() {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->scene().pending_damage_mask();
}

bool same_rect(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

std::string rect_text(lx::rect2i r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.width) + "x" +
           std::to_string(r.height);
}

/// The frame at which a critically damped spring from `x0` stops under `threshold`.
int stop_frame(double x0, double threshold) {
    for (int n = 0; n < 1000; ++n)
        if (crit_reach(x0, n * k_dt * 1e-9) < threshold)
            return n;
    return -1;
}

/// Frames of an open/close/minimize motion of a `w`x`h` frame at `rest`: per frame the
/// expected draw (scale about the centre, opacity), until it settles; `closing` runs
/// opacity 1 → 0 and scale 1 → 0.9, else 0 → 1 and 0.9 → 1.
struct pose {
    lx::rect2i dst{};
    double opacity = 1.0;
};
std::vector<pose> fade_frames(lx::rect2i rest, bool closing) {
    const double half = std::max(rest.width, rest.height) * 0.5;
    const int alpha_stop = stop_frame(1.0, 1.0 / 255.0);
    const int scale_stop = stop_frame(0.1, 0.5 / half);
    const int frames = std::max(alpha_stop, scale_stop);
    std::vector<pose> out;
    for (int n = 0; n < frames; ++n) {
        const double t = n * k_dt * 1e-9;
        // The value's offset from where it goes, decaying from 1 (opacity) / 0.1 (scale).
        const double a = n < alpha_stop ? crit_offset(1.0, t) : 0.0;
        const double sc = n < scale_stop ? crit_offset(0.1, t) : 0.0;
        const double opacity = closing ? a : 1.0 - a;
        const double s = closing ? 0.9 + sc : 1.0 - sc;
        const double cx = rest.x + rest.width * 0.5;
        const double cy = rest.y + rest.height * 0.5;
        pose p{};
        const long x0 = std::lround(cx - rest.width * s * 0.5);
        const long x1 = std::lround(cx + rest.width * s * 0.5);
        const long y0 = std::lround(cy - rest.height * s * 0.5);
        const long y1 = std::lround(cy + rest.height * s * 0.5);
        p.dst = {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
                 static_cast<int>(y1 - y0)};
        p.opacity = opacity;
        out.push_back(p);
    }
    return out;
}

/// Ticks through `want`, comparing each frame's draw; edges within `slack` px, opacity
/// within 1e-6. Returns the worst edge error (px) and opacity error, -1 on a missing draw.
struct frame_errors {
    int frames = 0;
    int worst_px = 0;
    double worst_opacity = 0.0;
    bool missing = false;
};
frame_errors run_frames(const std::vector<pose>& want) {
    frame_errors e{};
    for (const pose& p : want) {
        tick();
        const seen s = window_draw();
        ++e.frames;
        if (!s.found) {
            e.missing = true;
            continue;
        }
        const int d = std::max({std::abs(s.dst.x - p.dst.x), std::abs(s.dst.y - p.dst.y),
                                std::abs(s.dst.x + s.dst.width - p.dst.x - p.dst.width),
                                std::abs(s.dst.y + s.dst.height - p.dst.y - p.dst.height)});
        e.worst_px = std::max(e.worst_px, d);
        e.worst_opacity = std::max(e.worst_opacity, std::abs(s.opacity - p.opacity));
    }
    return e;
}

/// `run_frames` with a capture taken after its first frame: `cap` what it drew.
frame_errors run_frames_capturing(const std::vector<pose>& want, seen& cap) {
    frame_errors e = run_frames({want.begin(), want.begin() + 1});
    cap = capture_draw();
    const frame_errors more = run_frames({want.begin() + 1, want.end()});
    e.frames += more.frames;
    e.worst_px = std::max(e.worst_px, more.worst_px);
    e.worst_opacity = std::max(e.worst_opacity, more.worst_opacity);
    e.missing = e.missing || more.missing;
    return e;
}

std::string errors_text(const frame_errors& e) {
    char b[128];
    std::snprintf(b, sizeof(b), "%d frames, worst edge %d px, worst opacity %.2g%s", e.frames,
                  e.worst_px, e.worst_opacity, e.missing ? ", a frame without the window" : "");
    return b;
}

/// After a motion: one more tick settles it (lands at rest), the next has nothing to do.
bool goes_quiet(std::string& why) {
    tick();
    const bool still = animating();
    tick();
    const std::uint32_t dmg = pending_damage();
    tick();
    const bool quiet = !still && dmg == 0 && pending_damage() == 0 && !animating();
    if (!quiet)
        why = "animating=" + std::to_string(still) + " damage=" + std::to_string(dmg);
    return quiet;
}

void run_cases() {
    client c{};
    c.display = wl_display_connect(k_socket);
    if (!c.display) {
        check("connect", false);
        return;
    }
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    (void)wl_display_roundtrip(c.display);
    if (!c.compositor || !c.wm_base || !c.shm) {
        check("connect: wl_compositor, xdg_wm_base, wl_shm", false);
        return;
    }

    // Off (the default): a window is drawn at rest in its first frame; nothing animates.
    window a{};
    map_window(c, a, "a");
    tick();
    const rec_copy a0 = record("a");
    const seen sa = window_draw();
    check("off: a mapped window's first frame is at rest, nothing animates",
          a0.mapped && sa.found && sa.dst.x == a0.geometry.x && sa.dst.y == a0.geometry.y &&
              sa.dst.width == a0.geometry.width && sa.opacity == 1.f && !sa.scaled && !animating(),
          rect_text(sa.dst) + " rest " + rect_text(a0.geometry));
    destroy_window(c, a);
    tick();
    check("off: a closed window is gone in the next frame", !window_draw().found && !animating());

    with_wm([](lx::compositor::window_manager& wm) {
        wm.set_animation_config({true, k_stiffness, 1.0});
        return 0;
    });

    // Open: from opacity 0 at 0.9 of its size, every frame the closed form.
    window b{};
    map_window(c, b, "b");
    const rec_copy b0 = record("b");
    {
        const auto want = fade_frames(b0.geometry, false);
        seen cap{};
        const frame_errors e = run_frames_capturing(want, cap);
        check("open: every frame fades and grows as the critically damped spring says",
              !e.missing && e.worst_px <= 1 && e.worst_opacity < 1e-6, errors_text(e));
        check("open: a capture meanwhile shows it at rest",
              cap.found && same_rect(cap.dst, b0.geometry) && cap.opacity == 1.f && !cap.scaled,
              rect_text(cap.dst));
        std::string why;
        const bool quiet = goes_quiet(why);
        const seen s = window_draw();
        check("open: it lands exactly at rest, then the desktop is quiet",
              quiet && s.found && s.dst.x == b0.geometry.x &&
                  s.dst.width == b0.geometry.width && s.opacity == 1.f && !s.scaled,
              why + " last " + rect_text(s.dst));
    }

    // Close: the ghost fades out to 0.9 of its size, then is gone.
    destroy_window(c, b);
    {
        const auto want = fade_frames(b0.geometry, true);
        seen cap{};
        const frame_errors e = run_frames_capturing(want, cap);
        check("close: the ghost fades and shrinks frame by frame",
              !e.missing && e.worst_px <= 1 && e.worst_opacity < 1e-6, errors_text(e));
        check("close: a capture meanwhile shows nothing of the closed window", !cap.found,
              rect_text(cap.dst));
        std::string why;
        const bool quiet = goes_quiet(why);
        check("close: then nothing of it is drawn and the desktop is quiet",
              quiet && !window_draw().found, why);
    }

    // A workspace switch: the window slides out to the left (the next workspace comes from
    // the right), then is hidden; switching back slides it in from the left.
    window d{};
    map_window(c, d, "d");
    for (int i = 0; i < 40 && animating(); ++i)
        tick();
    const rec_copy d0 = record("d");
    const double W = 1280.0;
    const int slide_stop = stop_frame(W, 0.5);
    with_wm([](lx::compositor::window_manager& wm) { return wm.switch_to_index(2); });
    {
        int worst = 0;
        bool missing = false;
        bool captured = true;
        for (int n = 0; n < slide_stop; ++n) {
            tick();
            const seen s = window_draw();
            if (n == 0)
                captured = capture_draw().found;
            const double v = -W + crit_offset(W, n * k_dt * 1e-9);
            const long x = std::lround(d0.geometry.x + v);
            // Off the output entirely it is not emitted: nothing to compare.
            if (x + d0.geometry.width <= 0)
                continue;
            if (!s.found) {
                missing = true;
                continue;
            }
            worst = std::max({worst, static_cast<int>(std::abs(s.dst.x - x)),
                              std::abs(s.dst.y - d0.geometry.y),
                              std::abs(s.dst.width - d0.geometry.width)});
        }
        check("switch: the window slides out frame by frame, 1:1",
              !missing && worst <= 1, "worst " + std::to_string(worst) + " px over " +
                                          std::to_string(slide_stop) + " frames");
        check("switch: a capture meanwhile shows nothing of the workspace it leaves", !captured);
        std::string why;
        check("switch: then it is hidden and the desktop is quiet",
              goes_quiet(why) && !window_draw().found, why);
    }
    with_wm([](lx::compositor::window_manager& wm) { return wm.switch_to_index(1); });
    {
        int worst = 0;
        int frames = 0;
        for (int n = 0; n < slide_stop; ++n) {
            tick();
            const seen s = window_draw();
            // From −W toward 0.
            const long want = std::lround(d0.geometry.x - crit_offset(W, n * k_dt * 1e-9));
            if (want + d0.geometry.width <= 0)
                continue;
            ++frames;
            worst = s.found ? std::max(worst, static_cast<int>(std::abs(s.dst.x - want))) : 9999;
        }
        std::string why;
        const bool quiet = goes_quiet(why);
        const seen s = window_draw();
        check("switch back: it slides in from the left and lands at rest",
              worst <= 1 && frames > 0 && quiet && s.found && s.dst.x == d0.geometry.x,
              "worst " + std::to_string(worst) + " px; " + why);
    }

    // Minimize, then unminimize: fades out (hidden at the end), fades back in.
    with_wm([&](lx::compositor::window_manager& wm) {
        wm.set_minimized(d0.id, true, "test");
        return 0;
    });
    {
        seen cap{};
        const frame_errors e = run_frames_capturing(fade_frames(d0.geometry, true), cap);
        std::string why;
        const bool quiet = goes_quiet(why);
        check("minimize: fades out frame by frame, then hidden and quiet",
              !e.missing && e.worst_px <= 1 && e.worst_opacity < 1e-6 && quiet && !window_draw().found,
              errors_text(e) + "; " + why);
        check("minimize: a capture meanwhile shows nothing of it", !cap.found, rect_text(cap.dst));
    }
    with_wm([&](lx::compositor::window_manager& wm) {
        wm.set_minimized(d0.id, false, "test");
        return 0;
    });
    {
        const frame_errors e = run_frames(fade_frames(d0.geometry, false));
        std::string why;
        check("unminimize: fades back in frame by frame",
              !e.missing && e.worst_px <= 1 && e.worst_opacity < 1e-6 && goes_quiet(why),
              errors_text(e) + "; " + why);
    }

    // Maximize: the client is configured for the whole output at once; its new buffer is
    // drawn where the window was, then moves and grows to the output. The configures the
    // window got meanwhile (focus) are answered first: one is in flight at a time.
    for (int i = 0; i < 4; ++i) {
        (void)wl_display_roundtrip(c.display);
        if (d.serial != d.acked)
            answer(c, d);
        (void)wl_display_roundtrip(c.display);
        tick();
    }
    check("maximize: nothing animates before it", !animating());
    with_wm([&](lx::compositor::window_manager& wm) {
        wm.set_maximized(d0.id, true, "test");
        return 0;
    });
    (void)wl_display_roundtrip(c.display);
    check("maximize: the configure carries the final size at once",
          d.width == 1280 && d.height == 720,
          std::to_string(d.width) + "x" + std::to_string(d.height));
    answer(c, d);
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    {
        const lx::rect2i from = d0.geometry;
        const lx::rect2i to{0, 0, 1280, 720};
        // Each edge on its own spring from its old place.
        const int sx = stop_frame(std::abs(from.x - to.x), 0.5);
        const int sy = stop_frame(std::abs(from.y - to.y), 0.5);
        const int sw = stop_frame(std::abs(from.width - to.width), 0.5);
        const int sh = stop_frame(std::abs(from.height - to.height), 0.5);
        const int frames = std::max({sx, sy, sw, sh});
        int worst = 0;
        bool scaled_mid = true;
        bool first_at_old = false;
        for (int n = 0; n < frames; ++n) {
            tick();
            const seen s = window_draw();
            const double t = n * k_dt * 1e-9;
            const double x = to.x + (n < sx ? crit_offset(from.x - to.x, t) : 0.0);
            const double y = to.y + (n < sy ? crit_offset(from.y - to.y, t) : 0.0);
            const double w = to.width + (n < sw ? crit_offset(from.width - to.width, t) : 0.0);
            const double h = to.height + (n < sh ? crit_offset(from.height - to.height, t) : 0.0);
            if (!s.found) {
                worst = 9999;
                continue;
            }
            if (n == 0)
                first_at_old = s.dst.x == from.x && s.dst.y == from.y &&
                               s.dst.width == from.width && s.dst.height == from.height;
            worst = std::max({worst, static_cast<int>(std::abs(s.dst.x - std::lround(x))),
                              static_cast<int>(std::abs(s.dst.y - std::lround(y))),
                              static_cast<int>(std::abs(s.dst.x + s.dst.width - std::lround(x + w))),
                              static_cast<int>(std::abs(s.dst.y + s.dst.height - std::lround(y + h)))});
            if (n + 1 < std::min({sw, sh}))
                scaled_mid = scaled_mid && s.scaled;
        }
        check("maximize: the first frame shows the new buffer where the window was",
              first_at_old, rect_text(from));
        check("maximize: every frame moves and resizes as the springs say (resampled)",
              worst <= 1 && scaled_mid, "worst " + std::to_string(worst) + " px over " +
                                            std::to_string(frames) + " frames");
        std::string why;
        const bool quiet = goes_quiet(why);
        const seen s = window_draw();
        const rec_copy dm = record("d");
        check("maximize: it lands on the output 1:1 and the desktop is quiet",
              quiet && s.found && s.dst.x == 0 && s.dst.y == 0 && s.dst.width == 1280 &&
                  s.dst.height == 720 && !s.scaled && dm.geometry.width == 1280,
              why + " last " + rect_text(s.dst));
    }

    // Two placements in a row: maximize again, and restore once its top edge has settled
    // while its size still moves. The settled edge starts again with the next frame, from
    // where it is shown: first frame in place, then the closed form toward the restored rect.
    with_wm([&](lx::compositor::window_manager& wm) {
        wm.set_maximized(d0.id, false, "test");
        return 0;
    });
    (void)wl_display_roundtrip(c.display);
    answer(c, d);
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    for (int i = 0; i < 60 && animating(); ++i)
        tick();
    const rec_copy dr = record("d");
    with_wm([&](lx::compositor::window_manager& wm) {
        wm.set_maximized(d0.id, true, "test");
        return 0;
    });
    (void)wl_display_roundtrip(c.display);
    answer(c, d);
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    {
        const lx::rect2i from = dr.geometry;
        const int sy = stop_frame(std::abs(from.y), 0.5);
        const int sw = stop_frame(std::abs(from.width - 1280), 0.5);
        const int sh = stop_frame(std::abs(from.height - 720), 0.5);
        const int still = std::max(sw, sh);
        // Two frames after the top edge settled (it stops at its sample `sy`), its size
        // still moving.
        const int restore_at = sy + 2;
        for (int n = 0; n <= restore_at; ++n)
            tick();
        with_wm([&](lx::compositor::window_manager& wm) {
            wm.set_maximized(d0.id, false, "test");
            return 0;
        });
        (void)wl_display_roundtrip(c.display);
        answer(c, d);
        (void)wl_display_roundtrip(c.display);
        (void)wl_display_roundtrip(c.display);
        const rec_copy back = record("d");
        // The top edge: from 0 toward the restored y.
        const int frames = stop_frame(std::abs(back.geometry.y), 0.5);
        int worst = 0;
        int first = 9999;
        for (int k = 0; k < frames; ++k) {
            tick();
            const seen s = window_draw();
            const long y = std::lround(back.geometry.y +
                                       crit_offset(0.0 - back.geometry.y, k * k_dt * 1e-9));
            const int e = s.found ? static_cast<int>(std::abs(s.dst.y - y)) : 9999;
            if (k == 0)
                first = s.found ? s.dst.y : 9999;
            worst = std::max(worst, e);
        }
        check("maximize, then restore in its tail: the settled edge stays put, then springs",
              restore_at < still && same_rect(back.geometry, from) && first == 0 &&
                  worst <= 1,
              "top edge settled at frame " + std::to_string(sy) + ", size at " +
                  std::to_string(still) + "; first frame y " + std::to_string(first) +
                  ", worst " + std::to_string(worst) + " px over " + std::to_string(frames) +
                  " frames");
        // Its size, retargeted while it moved, takes longer.
        for (int i = 0; i < 60 && animating(); ++i)
            tick();
        std::string why;
        const bool quiet = goes_quiet(why);
        const seen s = window_draw();
        check("restore: it lands at rest 1:1 and the desktop is quiet",
              quiet && s.found && same_rect(s.dst, back.geometry) && !s.scaled,
              why + " last " + rect_text(s.dst));
    }

    // Turned off mid-motion: it ends at once.
    destroy_window(c, d);
    tick();
    const bool was = animating();
    with_wm([](lx::compositor::window_manager& wm) {
        wm.set_animation_config({false, k_stiffness, 1.0});
        return 0;
    });
    check("off mid-motion: every motion ends at once", was && !animating());
    tick();
    check("off mid-motion: the ghost is gone", !window_draw().found);
    wl_display_disconnect(c.display);
}

} // namespace

int run_wm_cases() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 60.0;
    cfg.virtual_outputs = "1280x720";
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    std::atomic<bool> pump{true};
    std::mutex dispatch_lock;
    g_comp = &comp;
    g_lock = &dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int loop_fd = comp.wayland().event_fd();
        while (pump.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(0);
                comp.windows().flush_workspaces();
                comp.wayland().flush();
            }
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 10);
        }
    });
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    run_cases();

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d window-manager cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif

int main(int argc, char** argv) {
    const int unit = lumen_test::run_all(argc, argv);
    const int wm = run_wm_cases();
    return unit != 0 || wm != 0 ? 1 : 0;
}
