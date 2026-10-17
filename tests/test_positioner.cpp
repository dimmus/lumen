// xdg_positioner placement (base rectangle, flip → slide → resize) and the popup manager's
// configure / map / grab / dismissal state machine. Pure — no Wayland: the popup manager runs
// over a real surface tree, surface manager and toplevel manager, with recording wire ops.
#include "lumen_test.hpp"

#include <memory>

import lx.foundation;
import lx.runtime;
import lx.gfx;
import lx.scene;
import lx.compositor;

using lx::compositor::popup_manager;
using lx::compositor::popup_parent_kind;
using lx::compositor::positioner_base;
using lx::compositor::positioner_place;
using lx::compositor::positioner_rules;

namespace {

bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

positioner_rules rules(lx::size2i size, lx::rect2i anchor_rect, unsigned anchor, unsigned gravity,
                       unsigned adjust = 0, lx::point2i offset = {}) {
    positioner_rules r{};
    r.size = size;
    r.size_set = true;
    r.anchor_rect = anchor_rect;
    r.anchor_rect_set = true;
    r.anchor = anchor;
    r.gravity = gravity;
    r.adjustment = adjust;
    r.offset = offset;
    return r;
}

// Anchor/gravity wire values.
constexpr unsigned none = 0, top = 1, bottom = 2, left = 3, right = 4, top_left = 5,
                   bottom_left = 6, top_right = 7, bottom_right = 8;

} // namespace

// ── base rectangle ───────────────────────────────────────────────────────────

LUMEN_TEST(positioner_base_all_anchor_gravity_pairs) {
    // Anchor rect 10,20 50x31 (odd height: centre truncates), popup 21x11 (odd: half truncates).
    const lx::rect2i ar{10, 20, 50, 31};
    const lx::size2i sz{21, 11};
    // Anchor points per anchor value.
    const int ax[9] = {35, 35, 35, 10, 60, 10, 10, 60, 60};
    const int ay[9] = {35, 20, 51, 35, 35, 20, 51, 20, 51};
    // Gravity offsets from the anchor point.
    const int gx[9] = {-10, -10, -10, -21, 0, -21, -21, 0, 0};
    const int gy[9] = {-5, -11, 0, -5, -5, -11, 0, -11, 0};
    for (unsigned a = 0; a < 9; ++a) {
        for (unsigned g = 0; g < 9; ++g) {
            const lx::rect2i b = positioner_base(rules(sz, ar, a, g));
            LUMEN_CHECK(same(b, {ax[a] + gx[g], ay[a] + gy[g], 21, 11}));
        }
    }
}

LUMEN_TEST(positioner_base_adds_the_offset) {
    const lx::rect2i b =
        positioner_base(rules({100, 40}, {10, 10, 50, 20}, bottom_left, bottom_right, 0, {3, -7}));
    LUMEN_CHECK(same(b, {13, 23, 100, 40}));
}

LUMEN_TEST(positioner_complete_needs_positive_size_and_anchor_rect) {
    positioner_rules r{};
    LUMEN_CHECK(!r.complete());
    r.size = {10, 10};
    r.size_set = true;
    LUMEN_CHECK(!r.complete());
    r.anchor_rect = {0, 0, 0, 5};
    r.anchor_rect_set = true;
    LUMEN_CHECK(!r.complete()); // a zero-width anchor rect is not "non-zero"
    r.anchor_rect = {0, 0, 1, 1};
    LUMEN_CHECK(r.complete());
    r.size = {0, 10};
    LUMEN_CHECK(!r.complete());
}

// ── constraint adjustment ───────────────────────────────────────────────────

LUMEN_TEST(positioner_no_adjustment_leaves_a_constrained_popup) {
    const auto p = positioner_place(rules({200, 100}, {900, 10, 50, 20}, bottom_right, bottom_right),
                                    {0, 0, 1000, 800});
    LUMEN_CHECK(same(p.rect, {950, 30, 200, 100}));
    LUMEN_CHECK(p.flipped == 0 && p.slid == 0 && p.resized == 0);
}

LUMEN_TEST(positioner_flip_x_succeeds) {
    // Opens to the right of the anchor rect; no room: flipped to its left.
    const auto p = positioner_place(rules({200, 100}, {900, 10, 50, 20}, right, right, 4),
                                    {0, 0, 1000, 800});
    LUMEN_CHECK(p.flipped == 1);
    LUMEN_CHECK(same(p.rect, {700, -30, 200, 100}));
    LUMEN_CHECK(p.rect.x + p.rect.width == 900); // right edge on the anchor rect's left edge
}

LUMEN_TEST(positioner_flip_y_succeeds_and_keeps_the_original_offset) {
    // bottom anchor + bottom gravity with offset +5: flipped to top/top with the same +5.
    const auto p = positioner_place(
        rules({100, 300}, {100, 600, 40, 20}, bottom, bottom, 8, {0, 5}), {0, 0, 1000, 800});
    LUMEN_CHECK(p.flipped == 2);
    LUMEN_CHECK(same(p.rect, {70, 600 - 300 + 5, 100, 300}));
}

LUMEN_TEST(positioner_flip_that_is_still_constrained_reverts) {
    // Too tall either way: the flip would be constrained too, so the original stays.
    const auto p = positioner_place(rules({100, 700}, {100, 400, 40, 20}, bottom, bottom, 8),
                                    {0, 0, 1000, 800});
    LUMEN_CHECK(p.flipped == 0);
    LUMEN_CHECK(same(p.rect, {70, 420, 100, 700}));
}

LUMEN_TEST(positioner_slide_from_each_side) {
    const lx::rect2i c{0, 0, 1000, 800};
    // Past the right edge: slides left until it fits.
    auto p = positioner_place(rules({200, 100}, {900, 10, 50, 20}, bottom_left, bottom_right, 1), c);
    LUMEN_CHECK(p.slid == 1 && same(p.rect, {800, 30, 200, 100}));
    // Past the left edge.
    p = positioner_place(rules({200, 100}, {20, 10, 50, 20}, bottom_left, bottom_left, 1), c);
    LUMEN_CHECK(p.slid == 1 && same(p.rect, {0, 30, 200, 100}));
    // Past the bottom.
    p = positioner_place(rules({200, 100}, {20, 750, 50, 20}, bottom_left, bottom_right, 2), c);
    LUMEN_CHECK(p.slid == 2 && same(p.rect, {20, 700, 200, 100}));
    // Past the top.
    p = positioner_place(rules({200, 100}, {20, 10, 50, 20}, top_left, top_right, 2), c);
    LUMEN_CHECK(p.slid == 2 && same(p.rect, {20, 0, 200, 100}));
}

LUMEN_TEST(positioner_slide_wider_than_the_constraint_aligns_the_far_edge) {
    const lx::rect2i c{0, 0, 300, 800};
    // Left edge out, right edge in: slides right until the right edges meet — still
    // constrained on the left, as both of the spec's gravity-ordered phases end.
    auto p = positioner_place(rules({400, 50}, {50, 10, 10, 10}, top_left, bottom_left, 1), c);
    LUMEN_CHECK(p.slid == 1 && same(p.rect, {-100, 10, 400, 50}));
    // Right edge out, left edge in: slides left until the left edges meet.
    p = positioner_place(rules({400, 50}, {50, 10, 10, 10}, top_left, bottom_right, 1), c);
    LUMEN_CHECK(p.slid == 1 && same(p.rect, {0, 10, 400, 50}));
    // Out on both sides: nothing to gain, no move.
    p = positioner_place(rules({400, 50}, {140, 10, 10, 10}, top, bottom, 1), c);
    LUMEN_CHECK(p.slid == 0 && same(p.rect, {-55, 10, 400, 50}));
}

LUMEN_TEST(positioner_resize_clips_and_ignores_no_overlap) {
    const lx::rect2i c{0, 0, 1000, 800};
    auto p = positioner_place(rules({200, 100}, {900, 10, 50, 20}, bottom_right, bottom_right, 16), c);
    LUMEN_CHECK(p.resized == 1 && same(p.rect, {950, 30, 50, 100}));
    p = positioner_place(rules({200, 100}, {20, 750, 50, 20}, bottom_left, bottom_right, 32), c);
    LUMEN_CHECK(p.resized == 2 && same(p.rect, {20, 770, 200, 30}));
    // Entirely outside: no overlap to keep, so unchanged.
    p = positioner_place(rules({200, 100}, {1100, 10, 50, 20}, bottom_right, bottom_right, 16), c);
    LUMEN_CHECK(p.resized == 0 && same(p.rect, {1150, 30, 200, 100}));
}

LUMEN_TEST(positioner_precedence_flip_then_slide_then_resize) {
    const lx::rect2i c{0, 0, 1000, 800};
    // x: flip fails (no room on the left either), slide fits it.
    auto p = positioner_place(rules({700, 100}, {600, 10, 50, 20}, right, right, 1 | 4), c);
    LUMEN_CHECK(p.flipped == 0 && p.slid == 1 && same(p.rect, {300, -30, 700, 100}));
    // x: flip succeeds, so slide is not needed even though it is allowed.
    p = positioner_place(rules({200, 100}, {900, 10, 50, 20}, right, right, 1 | 4), c);
    LUMEN_CHECK(p.flipped == 1 && p.slid == 0);
    // y: flip fails, slide cannot (taller than the space), resize clips what is left.
    p = positioner_place(rules({100, 900}, {100, 300, 40, 20}, bottom, bottom, 2 | 8 | 32), c);
    LUMEN_CHECK(p.flipped == 0 && p.slid == 2 && p.resized == 2);
    LUMEN_CHECK(same(p.rect, {70, 0, 100, 800}));
}

LUMEN_TEST(positioner_empty_constraint_leaves_the_base) {
    const auto p = positioner_place(rules({200, 100}, {900, 10, 50, 20}, right, right, 63), {});
    LUMEN_CHECK(same(p.rect, {950, -30, 200, 100}) && p.flipped == 0 && p.slid == 0 && p.resized == 0);
}

// ── popup manager ────────────────────────────────────────────────────────────

namespace {

struct wire_log {
    unsigned configures = 0;
    lx::rect2i last_rel{};
    unsigned last_serial = 0;
    bool last_repositioned = false;
    unsigned last_token = 0;
    void* done[16]{};
    unsigned done_count = 0;
    unsigned serial = 100;
    unsigned focus_changes = 0;
};

wire_log* g_wire = nullptr;

struct fixture {
    lx::compositor::surface_manager surfaces{};
    lx::compositor::toplevel_manager toplevels{};
    lx::scene::scene_graph scene{};
    lx::scene::container_node windows{};
    lx::scene::container_node popup_layer{};
    lx::compositor::surface_tree tree{};
    popup_manager popups{};
    wire_log wire{};
    lx::toplevel_id window{};
    lx::surface_id window_surface{};
    unsigned long long next_buffer = 1;
    // Distinct non-null resource tokens per popup.
    int res[64]{};

    fixture() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        g_wire = &wire;
        surfaces.set_scene(&scene);
        tree.bind(surfaces, scene);
        (void)scene.root().add(&windows);
        (void)scene.root().add(&popup_layer);
        popups.bind(toplevels, surfaces, tree, popup_layer);
        lx::compositor::popup_ops ops{};
        ops.send_configure = [](void*, void*, lx::rect2i rel, unsigned serial, bool repositioned,
                                unsigned token) {
            ++g_wire->configures;
            g_wire->last_rel = rel;
            g_wire->last_serial = serial;
            g_wire->last_repositioned = repositioned;
            g_wire->last_token = token;
        };
        ops.send_popup_done = [](void* r) {
            if (g_wire->done_count < 16)
                g_wire->done[g_wire->done_count++] = r;
        };
        ops.next_serial = [](void*) { return ++g_wire->serial; };
        ops.client_alive = [](void*) { return true; };
        popups.set_ops(ops);
        lx::compositor::popup_host host{};
        host.constraint_area = [](void*, popup_parent_kind, lx::surface_id) {
            return lx::rect2i{0, 0, 1000, 800};
        };
        host.focus_changed = [](void*) { ++g_wire->focus_changes; };
        popups.set_host(host);

        // A mapped toplevel whose window geometry is at 100,100 400x300.
        window_surface = make(1);
        auto created = toplevels.create(lx::client_id{1}, window_surface, &res[0], &res[0], &res[0]);
        LUMEN_CHECK(static_cast<bool>(created));
        window = created.value();
        auto* rec = toplevels.find_mut(window);
        rec->geometry = {100, 100, 400, 300};
        toplevels.set_mapped(window, true);
    }

    lx::surface_id make(unsigned n) {
        const lx::surface_id s{n};
        LUMEN_CHECK(surfaces.register_surface(s, &res[n % 64]));
        LUMEN_CHECK(tree.add_surface(s));
        return s;
    }

    void content(lx::surface_id s, int w, int h) {
        const lx::buffer_id b{next_buffer++};
        surfaces.register_shm(b, static_cast<unsigned>(w), static_cast<unsigned>(h),
                              static_cast<lx::fourcc>(lx::pixel_format::argb8888));
        lx::compositor::surface_commit c{};
        c.surface = s;
        c.buffer = b;
        c.damage = {0, 0, w, h};
        LUMEN_CHECK(static_cast<bool>(surfaces.on_commit(static_cast<lx::compositor::surface_commit&&>(c))));
        tree.set_has_buffer(s, true);
    }

    /// get_popup + initial commit; returns the popup's surface.
    lx::surface_id popup(unsigned n, popup_parent_kind kind, lx::surface_id parent,
                         const positioner_rules& r) {
        const lx::surface_id s = make(n);
        LUMEN_CHECK(popups.create(s, lx::client_id{1}, &res[0], &res[n % 64], &res[n % 64],
                                  &res[n % 64], kind, parent, window, r) ==
                    popup_manager::create_error::none);
        popups.on_commit(s, {}); // initial commit → configure
        return s;
    }

    /// ack the last configure, then commit content of the configured size.
    void map(lx::surface_id s) {
        LUMEN_CHECK(popups.on_ack(s, wire.last_serial) == popup_manager::ack_result::ok);
        const lx::rect2i rel = wire.last_rel;
        content(s, rel.width, rel.height);
        lx::compositor::popup_commit_info info{};
        info.has_buffer = true;
        info.window_geometry = {0, 0, rel.width, rel.height};
        popups.on_commit(s, info);
        tree.sync_scene(s, "commit");
    }
};

std::unique_ptr<fixture> make_fixture() { return std::make_unique<fixture>(); }

positioner_rules menu_rules() {
    return rules({120, 200}, {10, 10, 50, 20}, bottom_left, bottom_right, 63);
}

} // namespace

LUMEN_TEST(popup_initial_commit_configures_and_ack_commit_maps_at_parent_relative_position) {
    auto f = make_fixture();
    const auto p = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    LUMEN_CHECK(f->wire.configures == 1);
    LUMEN_CHECK(same(f->wire.last_rel, {10, 30, 120, 200}));
    f->map(p);
    const auto* rec = f->popups.find(p);
    LUMEN_CHECK(rec && rec->mapped);
    LUMEN_CHECK(rec->origin.x == 110 && rec->origin.y == 130);
    LUMEN_CHECK(same(f->surfaces.node_rect(p), {110, 130, 120, 200}));
    LUMEN_CHECK(f->popups.mapped_count() == 1);
}

LUMEN_TEST(popup_constraint_is_parent_relative) {
    auto f = make_fixture();
    // Anchored near the parent's right edge, whose global right is 500; the output is 1000
    // wide: a 600-wide popup at rel x=390 (global 490) would end at 1090 → slides to 400.
    const auto p = f->popup(10, popup_parent_kind::toplevel, f->window_surface,
                            rules({600, 50}, {390, 10, 10, 10}, bottom_left, bottom_right, 1));
    (void)p;
    LUMEN_CHECK(same(f->wire.last_rel, {300, 20, 600, 50}));
}

LUMEN_TEST(popup_ack_rules) {
    auto f = make_fixture();
    const auto p = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    const unsigned s1 = f->wire.last_serial;
    LUMEN_CHECK(f->popups.on_ack(p, s1 + 50) == popup_manager::ack_result::invalid);
    LUMEN_CHECK(f->popups.on_ack(p, s1) == popup_manager::ack_result::ok);
    LUMEN_CHECK(f->popups.on_ack(p, s1) == popup_manager::ack_result::duplicate);
}

LUMEN_TEST(popup_reposition_applies_only_at_the_commit_after_the_ack) {
    auto f = make_fixture();
    const auto p = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    f->map(p);
    auto r = menu_rules();
    r.anchor_rect = {50, 50, 10, 10};
    f->popups.reposition(p, r, 7);
    LUMEN_CHECK(f->wire.last_repositioned && f->wire.last_token == 7);
    LUMEN_CHECK(same(f->wire.last_rel, {50, 60, 120, 200}));
    // Not yet: still where it was.
    LUMEN_CHECK(f->popups.find(p)->origin.x == 110);
    // A commit without the ack does not move it either.
    lx::compositor::popup_commit_info info{};
    info.has_buffer = true;
    info.window_geometry = {0, 0, 120, 200};
    f->popups.on_commit(p, info);
    LUMEN_CHECK(f->popups.find(p)->origin.x == 110);
    LUMEN_CHECK(f->popups.on_ack(p, f->wire.last_serial) == popup_manager::ack_result::ok);
    f->popups.on_commit(p, info);
    LUMEN_CHECK(f->popups.find(p)->origin.x == 150 && f->popups.find(p)->origin.y == 160);
}

LUMEN_TEST(popup_follows_its_parent_and_nested_popups_follow_theirs) {
    auto f = make_fixture();
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    f->map(a);
    const auto b = f->popup(11, popup_parent_kind::popup, a,
                            rules({80, 40}, {119, 0, 1, 20}, top_right, bottom_right, 0));
    f->map(b);
    LUMEN_CHECK(f->popups.find(b)->origin.x == 110 + 120 && f->popups.find(b)->origin.y == 130);
    f->toplevels.find_mut(f->window)->geometry.x += 25;
    f->popups.on_parent_moved(f->window_surface);
    LUMEN_CHECK(f->popups.find(a)->origin.x == 135);
    LUMEN_CHECK(f->popups.find(b)->origin.x == 255);
    LUMEN_CHECK(f->surfaces.node_rect(b).x == 255);
}

LUMEN_TEST(popup_map_is_deferred_while_the_parent_is_unmapped) {
    auto f = make_fixture();
    f->toplevels.set_mapped(f->window, false);
    const auto p = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    f->map(p);
    LUMEN_CHECK(!f->popups.find(p)->mapped);
}

LUMEN_TEST(popup_grab_chain_dismisses_top_down) {
    auto f = make_fixture();
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    LUMEN_CHECK(f->popups.grab(a, true, false) == popup_manager::grab_result::ok);
    f->map(a);
    const auto b = f->popup(11, popup_parent_kind::popup, a, menu_rules());
    LUMEN_CHECK(f->popups.grab(b, true, false) == popup_manager::grab_result::ok);
    f->map(b);
    const auto c = f->popup(12, popup_parent_kind::popup, b, menu_rules());
    LUMEN_CHECK(f->popups.grab(c, true, false) == popup_manager::grab_result::ok);
    f->map(c);
    // A tooltip (no grab) on the middle popup.
    const auto t = f->popup(13, popup_parent_kind::popup, b, menu_rules());
    f->map(t);
    LUMEN_CHECK(f->popups.keyboard_target() && f->popups.keyboard_target()->surface == c);
    f->popups.dismiss_all("outside-click");
    // c (top of the stack), then b's other child t, then b, then a.
    LUMEN_CHECK(f->wire.done_count == 4);
    LUMEN_CHECK(f->wire.done[0] == &f->res[12]);
    LUMEN_CHECK(f->wire.done[1] == &f->res[13]);
    LUMEN_CHECK(f->wire.done[2] == &f->res[11]);
    LUMEN_CHECK(f->wire.done[3] == &f->res[10]);
    LUMEN_CHECK(!f->popups.grab_active() && f->popups.keyboard_target() == nullptr);
    LUMEN_CHECK(f->popups.mapped_count() == 0);
}

LUMEN_TEST(popup_grab_errors_and_denials) {
    auto f = make_fixture();
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    f->map(a);
    // Mapped: too late to grab.
    LUMEN_CHECK(f->popups.grab(a, true, false) == popup_manager::grab_result::invalid_grab);
    // a took no explicit grab, so its child may not grab.
    const auto b = f->popup(11, popup_parent_kind::popup, a, menu_rules());
    LUMEN_CHECK(f->popups.grab(b, true, false) == popup_manager::grab_result::invalid_parent);
    // A bad serial is a denial, not an error: dismissed at once.
    const auto c = f->popup(12, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    LUMEN_CHECK(f->popups.grab(c, false, false) == popup_manager::grab_result::denied);
    LUMEN_CHECK(f->popups.find(c)->dismissed && f->wire.done_count == 1);
    // An interactive move/resize running: denied too.
    const auto d = f->popup(13, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    LUMEN_CHECK(f->popups.grab(d, true, true) == popup_manager::grab_result::denied);
    // The child of a dismissed grabbing popup is dismissed at once.
    const auto e = f->popup(14, popup_parent_kind::popup, c, menu_rules());
    LUMEN_CHECK(f->popups.grab(e, true, false) == popup_manager::grab_result::denied);
    LUMEN_CHECK(f->popups.find(e)->dismissed);
}

LUMEN_TEST(popup_grab_on_a_non_topmost_grabbing_parent_is_an_error) {
    auto f = make_fixture();
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    LUMEN_CHECK(f->popups.grab(a, true, false) == popup_manager::grab_result::ok);
    f->map(a);
    const auto b = f->popup(11, popup_parent_kind::popup, a, menu_rules());
    LUMEN_CHECK(f->popups.grab(b, true, false) == popup_manager::grab_result::ok);
    const auto c = f->popup(12, popup_parent_kind::popup, a, menu_rules());
    LUMEN_CHECK(f->popups.grab(c, true, false) == popup_manager::grab_result::not_topmost);
}

LUMEN_TEST(popup_destroying_the_top_returns_the_grab_to_its_parent) {
    auto f = make_fixture();
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    LUMEN_CHECK(f->popups.grab(a, true, false) == popup_manager::grab_result::ok);
    f->map(a);
    const auto b = f->popup(11, popup_parent_kind::popup, a, menu_rules());
    LUMEN_CHECK(f->popups.grab(b, true, false) == popup_manager::grab_result::ok);
    f->map(b);
    LUMEN_CHECK(f->popups.has_live_children(a));
    f->popups.destroy(b);
    LUMEN_CHECK(!f->popups.has_live_children(a));
    LUMEN_CHECK(f->popups.grab_active());
    LUMEN_CHECK(f->popups.keyboard_target() && f->popups.keyboard_target()->surface == a);
    f->popups.destroy(a);
    LUMEN_CHECK(!f->popups.grab_active());
    LUMEN_CHECK(f->wire.done_count == 0); // destroyed by the client, not dismissed
}

LUMEN_TEST(popup_parent_unmapped_dismisses_every_descendant) {
    auto f = make_fixture();
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    f->map(a);
    const auto b = f->popup(11, popup_parent_kind::popup, a, menu_rules());
    f->map(b);
    f->popups.on_parent_unmapped(f->window_surface, "parent-unmapped");
    LUMEN_CHECK(f->wire.done_count == 2);
    LUMEN_CHECK(f->wire.done[0] == &f->res[11] && f->wire.done[1] == &f->res[10]);
    LUMEN_CHECK(f->popups.mapped_count() == 0);
}

LUMEN_TEST(popup_null_buffer_unmaps_and_needs_a_new_initial_commit) {
    auto f = make_fixture();
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, menu_rules());
    f->map(a);
    lx::compositor::popup_commit_info none{};
    f->popups.on_commit(a, none);
    LUMEN_CHECK(!f->popups.find(a)->mapped && !f->popups.find(a)->initial_commit_seen);
    const unsigned before = f->wire.configures;
    f->popups.on_commit(a, none);
    LUMEN_CHECK(f->wire.configures == before + 1);
}

LUMEN_TEST(popup_reactive_is_reconfigured_when_its_parent_moves) {
    auto f = make_fixture();
    auto r = rules({200, 100}, {390, 10, 10, 10}, bottom_left, bottom_right, 1);
    r.reactive = true;
    const auto a = f->popup(10, popup_parent_kind::toplevel, f->window_surface, r);
    f->map(a); // rel x = 390 fits: global 490..690
    const unsigned before = f->wire.configures;
    f->toplevels.find_mut(f->window)->geometry.x = 700; // now 1090..1290: must slide
    f->popups.on_parent_moved(f->window_surface);
    LUMEN_CHECK(f->wire.configures == before + 1 && !f->wire.last_repositioned);
    LUMEN_CHECK(f->wire.last_rel.x == 100); // 700 + 100 + 200 = 1000
    // It keeps its relative position until the client acks and commits.
    LUMEN_CHECK(f->popups.find(a)->origin.x == 700 + 390);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
