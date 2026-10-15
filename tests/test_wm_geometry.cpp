#include "lumen_test.hpp"

import lx.foundation;
import lx.compositor;

using lx::compositor::toplevel_configure;
using lx::compositor::toplevel_manager;
using lx::compositor::toplevel_record;

namespace {

bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

toplevel_configure anchored(unsigned edges, lx::rect2i start) {
    toplevel_configure c{};
    c.anchor_edges = edges;
    c.anchor = start;
    return c;
}

constexpr long long k_ms = 1'000'000;

} // namespace

// ── anchoring ────────────────────────────────────────────────────────────────

LUMEN_TEST(wm_anchor_all_eight_edges_keep_the_opposite_side_fixed) {
    using namespace lx::compositor;
    const lx::rect2i start{100, 200, 300, 200};
    const lx::size2i grown{350, 260};
    // Right and bottom edges of `start` are at x=400, y=400.
    struct expect {
        unsigned edges;
        int x, y;
    };
    const expect cases[] = {
        {edge_top, 100, 400 - 260},
        {edge_bottom, 100, 200},
        {edge_left, 400 - 350, 200},
        {edge_right, 100, 200},
        {edge_top | edge_left, 400 - 350, 400 - 260},
        {edge_top | edge_right, 100, 400 - 260},
        {edge_bottom | edge_left, 400 - 350, 200},
        {edge_bottom | edge_right, 100, 200},
    };
    for (const auto& c : cases) {
        const lx::rect2i got = anchored_geometry(start, anchored(c.edges, start), grown);
        LUMEN_CHECK(got.x == c.x);
        LUMEN_CHECK(got.y == c.y);
        LUMEN_CHECK(got.width == 350 && got.height == 260);
    }
}

LUMEN_TEST(wm_anchor_uses_the_committed_size_not_the_configured_one) {
    using namespace lx::compositor;
    // A terminal asked for 333 px wide answers with 328 (whole cells): its right edge must
    // still sit exactly where it was when resizing from the left.
    const lx::rect2i start{50, 50, 400, 300};
    const lx::rect2i got = anchored_geometry(start, anchored(edge_left, start), {328, 300});
    LUMEN_CHECK(got.x + got.width == start.x + start.width);
}

LUMEN_TEST(wm_anchor_without_edges_keeps_the_position) {
    const lx::rect2i current{10, 20, 30, 40};
    const lx::rect2i got = lx::compositor::anchored_geometry(current, toplevel_configure{}, {99, 77});
    LUMEN_CHECK(same(got, {10, 20, 99, 77}));
}

// ── window geometry ──────────────────────────────────────────────────────────

LUMEN_TEST(wm_window_geometry_defaults_to_the_surface_and_clamps_to_it) {
    using lx::compositor::effective_window_geometry;
    const lx::rect2i surface{0, 0, 640, 480};
    LUMEN_CHECK(same(effective_window_geometry(false, {}, surface), {0, 0, 640, 480}));
    // CSD shadow: 24 px margin all round.
    LUMEN_CHECK(same(effective_window_geometry(true, {24, 24, 592, 432}, surface),
                     {24, 24, 592, 432}));
    // Larger than the surface: intersected with it.
    LUMEN_CHECK(same(effective_window_geometry(true, {-10, -10, 1000, 1000}, surface),
                     {0, 0, 640, 480}));
    // Entirely outside: meaningless, so the whole surface.
    LUMEN_CHECK(same(effective_window_geometry(true, {700, 700, 10, 10}, surface),
                     {0, 0, 640, 480}));
    // No content at all: nothing.
    LUMEN_CHECK(same(effective_window_geometry(false, {}, {}), {}));
    LUMEN_CHECK(same(effective_window_geometry(true, {0, 0, 10, 10}, {}), {}));
}

LUMEN_TEST(wm_window_geometry_includes_subsurfaces) {
    using lx::compositor::effective_window_geometry;
    // foot-style CSD: a 30 px title bar subsurface above the main surface and 5 px border
    // subsurfaces around both — the tree's bounding box reaches above and left of 0,0.
    const lx::rect2i tree{-5, -35, 650, 520};
    // Unset: the whole tree, negative origin included.
    LUMEN_CHECK(same(effective_window_geometry(false, {}, tree), tree));
    // Set to the title bar plus main surface (no borders): kept, though y < 0.
    LUMEN_CHECK(same(effective_window_geometry(true, {0, -30, 640, 510}, tree),
                     {0, -30, 640, 510}));
    // Clamped to the tree, not to the main surface's buffer.
    LUMEN_CHECK(same(effective_window_geometry(true, {-20, -50, 700, 600}, tree), tree));
    // Clamped on one side only.
    LUMEN_CHECK(same(effective_window_geometry(true, {0, -40, 640, 400}, tree),
                     {0, -35, 640, 395}));
}

// ── placement ────────────────────────────────────────────────────────────────

LUMEN_TEST(wm_placement_centres_then_cascades) {
    using lx::compositor::place_new_window;
    const lx::rect2i output{0, 0, 1920, 1080};
    const lx::point2i first = place_new_window(output, {400, 300}, nullptr, 0);
    LUMEN_CHECK(first.x == 760 && first.y == 390);

    const lx::rect2i existing[] = {{760, 390, 400, 300}};
    const lx::point2i second = place_new_window(output, {400, 300}, existing, 1);
    LUMEN_CHECK(second.x == 792 && second.y == 422);

    const lx::rect2i two[] = {{760, 390, 400, 300}, {792, 422, 400, 300}};
    const lx::point2i third = place_new_window(output, {400, 300}, two, 2);
    LUMEN_CHECK(third.x == 824 && third.y == 454);
}

LUMEN_TEST(wm_placement_wraps_when_the_cascade_leaves_the_output) {
    using lx::compositor::place_new_window;
    const lx::rect2i output{0, 0, 1000, 800};
    // A window nearly the output's size: centred at (10,10); one cascade step runs off.
    const lx::rect2i existing[] = {{10, 10, 980, 780}};
    const lx::point2i p = place_new_window(output, {980, 780}, existing, 1);
    LUMEN_CHECK(p.x == 32 && p.y == 32);
}

LUMEN_TEST(wm_placement_of_an_oversized_window_starts_at_the_output_origin) {
    const lx::point2i p =
        lx::compositor::place_new_window({100, 0, 800, 600}, {1200, 900}, nullptr, 0);
    LUMEN_CHECK(p.x == 100 && p.y == 0);
}

LUMEN_TEST(wm_constrain_keeps_the_title_bar_and_a_grab_area_on_screen) {
    using lx::compositor::constrain_on_output;
    const lx::rect2i output{0, 0, 1920, 1080};
    // Off the top: pulled down so the top edge is on the output.
    lx::point2i p = constrain_on_output(output, {100, -50, 400, 300});
    LUMEN_CHECK(p.x == 100 && p.y == 0);
    // Off the right: at least 64 px stay visible.
    p = constrain_on_output(output, {1900, 100, 400, 300});
    LUMEN_CHECK(p.x == 1920 - 64);
    // Off the left: likewise.
    p = constrain_on_output(output, {-1000, 100, 400, 300});
    LUMEN_CHECK(p.x == -400 + 64);
    // Off the bottom: 32 px stay.
    p = constrain_on_output(output, {100, 2000, 400, 300});
    LUMEN_CHECK(p.y == 1080 - 32);
    // Inside: untouched.
    p = constrain_on_output(output, {100, 100, 400, 300});
    LUMEN_CHECK(p.x == 100 && p.y == 100);
}

// ── configure state machine ──────────────────────────────────────────────────

namespace {

toplevel_record* make_record(toplevel_manager& m) {
    auto id = m.create(lx::client_id{1}, lx::surface_id{1}, nullptr, nullptr, nullptr);
    LUMEN_CHECK(static_cast<bool>(id));
    return m.find_mut(id.value());
}

toplevel_configure sized(unsigned serial, int w, int h, unsigned states = 0) {
    toplevel_configure c{};
    c.serial = serial;
    c.size = {w, h};
    c.states = states;
    return c;
}

} // namespace

LUMEN_TEST(wm_configure_identical_request_is_never_resent) {
    toplevel_manager m{};
    auto* rec = make_record(m);
    rec->desired = sized(0, 300, 200, lx::compositor::tl_activated);
    LUMEN_CHECK(toplevel_manager::want_send(*rec, 0));
    toplevel_manager::on_sent(*rec, sized(5, 300, 200, lx::compositor::tl_activated), 0);
    LUMEN_CHECK(toplevel_manager::on_ack(*rec, 5) == toplevel_manager::ack_result::ok);
    toplevel_configure applied{};
    LUMEN_CHECK(toplevel_manager::consume_ack(*rec, applied));
    LUMEN_CHECK(!rec->inflight);
    // Nothing changed: no configure, however much time passes.
    LUMEN_CHECK(!toplevel_manager::want_send(*rec, 10'000 * k_ms));
    // Anchor fields are not part of the request.
    rec->desired.anchor_edges = lx::compositor::edge_left;
    LUMEN_CHECK(!toplevel_manager::want_send(*rec, 10'000 * k_ms));
}

LUMEN_TEST(wm_configure_single_in_flight_with_watchdog) {
    toplevel_manager m{};
    auto* rec = make_record(m);
    rec->desired = sized(0, 300, 200);
    toplevel_manager::on_sent(*rec, sized(1, 300, 200), 0);
    rec->desired = sized(0, 310, 200);
    // The first is still unanswered: the newer size waits, coalescing in `desired`.
    LUMEN_CHECK(!toplevel_manager::want_send(*rec, 50 * k_ms));
    rec->desired = sized(0, 320, 200);
    LUMEN_CHECK(!toplevel_manager::want_send(*rec, 199 * k_ms));
    // A client that never answers cannot wedge the pipeline.
    LUMEN_CHECK(toplevel_manager::want_send(*rec, 200 * k_ms));
    LUMEN_CHECK(toplevel_manager::timed_out(*rec, 200 * k_ms));

    // Ack + commit of the first releases the slot for the coalesced latest size.
    toplevel_record fresh{};
    fresh.desired = sized(0, 300, 200);
    toplevel_manager::on_sent(fresh, sized(1, 300, 200), 0);
    fresh.desired = sized(0, 320, 200);
    LUMEN_CHECK(toplevel_manager::on_ack(fresh, 1) == toplevel_manager::ack_result::ok);
    LUMEN_CHECK(!toplevel_manager::want_send(fresh, 1 * k_ms)); // acked but not committed
    toplevel_configure applied{};
    LUMEN_CHECK(toplevel_manager::consume_ack(fresh, applied));
    LUMEN_CHECK(applied.serial == 1 && applied.size.width == 300);
    LUMEN_CHECK(toplevel_manager::want_send(fresh, 1 * k_ms));
}

LUMEN_TEST(wm_ack_ok_duplicate_and_invalid) {
    toplevel_record rec{};
    toplevel_manager::on_sent(rec, sized(10, 100, 100), 0);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 9) == toplevel_manager::ack_result::invalid);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 11) == toplevel_manager::ack_result::invalid);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 10) == toplevel_manager::ack_result::ok);
    LUMEN_CHECK(rec.ever_acked && rec.acked_unapplied);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 10) == toplevel_manager::ack_result::duplicate);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 3) == toplevel_manager::ack_result::duplicate);
}

LUMEN_TEST(wm_ack_of_a_newer_serial_pops_every_older_one) {
    toplevel_record rec{};
    toplevel_manager::on_sent(rec, sized(1, 100, 100), 0);
    toplevel_manager::on_sent(rec, sized(2, 110, 100), 300 * k_ms); // watchdog resend
    toplevel_manager::on_sent(rec, sized(3, 120, 100), 600 * k_ms);
    LUMEN_CHECK(rec.sent_count == 3);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 2) == toplevel_manager::ack_result::ok);
    LUMEN_CHECK(rec.sent_count == 1 && rec.sent[0].serial == 3);
    LUMEN_CHECK(rec.acked.size.width == 110);
    toplevel_configure applied{};
    LUMEN_CHECK(toplevel_manager::consume_ack(rec, applied));
    // 3 is still outstanding, so the slot stays taken.
    LUMEN_CHECK(rec.inflight);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 1) == toplevel_manager::ack_result::duplicate);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 3) == toplevel_manager::ack_result::ok);
    LUMEN_CHECK(toplevel_manager::consume_ack(rec, applied));
    LUMEN_CHECK(!rec.inflight);
}

LUMEN_TEST(wm_ack_of_a_serial_dropped_on_overflow_is_tolerated) {
    toplevel_record rec{};
    for (unsigned s = 1; s <= 5; ++s)
        toplevel_manager::on_sent(rec, sized(s, 100 + static_cast<int>(s), 100), s * 300 * k_ms);
    LUMEN_CHECK(rec.sent_count == toplevel_record::k_sent_cap);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 1) == toplevel_manager::ack_result::duplicate);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 5) == toplevel_manager::ack_result::ok);
}

LUMEN_TEST(wm_pending_state_rejects_min_above_max_and_applies_atomically) {
    toplevel_record rec{};
    rec.min_pending = {200, 100};
    rec.min_pending_set = true;
    rec.max_pending = {100, 0}; // width contradicts, height unlimited
    rec.max_pending_set = true;
    rec.window_geometry_pending = {1, 2, 3, 4};
    rec.window_geometry_pending_set = true;
    LUMEN_CHECK(!toplevel_manager::apply_pending_state(rec));
    LUMEN_CHECK(!rec.window_geometry_set); // nothing applied

    rec.max_pending = {0, 0};
    LUMEN_CHECK(toplevel_manager::apply_pending_state(rec));
    LUMEN_CHECK(rec.min_size.width == 200 && rec.max_size.width == 0);
    LUMEN_CHECK(rec.window_geometry_set && same(rec.window_geometry_requested, {1, 2, 3, 4}));
}

// ── stacking ─────────────────────────────────────────────────────────────────

LUMEN_TEST(wm_stacking_order_survives_slot_reuse) {
    toplevel_manager m{};
    auto a = m.create(lx::client_id{1}, lx::surface_id{1}, nullptr, nullptr, nullptr).value();
    auto b = m.create(lx::client_id{1}, lx::surface_id{2}, nullptr, nullptr, nullptr).value();
    auto c = m.create(lx::client_id{1}, lx::surface_id{3}, nullptr, nullptr, nullptr).value();
    m.set_mapped(a, true);
    m.set_mapped(b, true);
    m.set_mapped(c, true);
    LUMEN_CHECK(m.top() == c);
    LUMEN_CHECK(m.raise(a));
    LUMEN_CHECK(m.top() == a && m.stacking_index(b) == 0 && m.stacking_index(c) == 1);
    m.destroy(c);
    LUMEN_CHECK(m.mapped_count() == 2 && m.stacking_index(b) == 0 && m.stacking_index(a) == 1);
    // c's slot is reused; the new record is a different id and starts unmapped.
    auto d = m.create(lx::client_id{2}, lx::surface_id{4}, nullptr, nullptr, nullptr).value();
    LUMEN_CHECK(!(d == c));
    LUMEN_CHECK(m.find(c) == nullptr);
    LUMEN_CHECK(m.stacking_index(d) == ~0u);
    unsigned seen = 0;
    lx::toplevel_id order[2]{};
    m.for_each_top_down([&](const toplevel_record& r) {
        order[seen++] = r.id;
        return true;
    });
    LUMEN_CHECK(seen == 2 && order[0] == a && order[1] == b);
}

// ── input region (wl_region add/subtract, hit-test predicate) ─────────────────

LUMEN_TEST(wm_input_region_default_is_infinite_and_wl_region_starts_empty) {
    lx::compositor::input_region surface_default{};
    LUMEN_CHECK(surface_default.contains(-5, 1'000'000));
    lx::compositor::input_region r{};
    r.reset_empty();
    LUMEN_CHECK(!r.contains(0, 0));
    r.add({10, 10, 0, 5}); // empty rects are ignored
    LUMEN_CHECK(r.count == 0);
}

LUMEN_TEST(wm_input_region_subtract_cuts_a_hole) {
    lx::compositor::input_region r{};
    r.reset_empty();
    r.add({0, 0, 100, 100});
    r.subtract({40, 40, 20, 20});
    LUMEN_CHECK(r.count == 4);
    LUMEN_CHECK(r.contains(0, 0) && r.contains(99, 99) && r.contains(39, 50) && r.contains(60, 50));
    LUMEN_CHECK(!r.contains(40, 40) && !r.contains(59, 59) && !r.contains(50, 50));
    LUMEN_CHECK(!r.contains(100, 50) && !r.contains(-1, 0));
    LUMEN_CHECK(!r.approximated);
    // A cut covering everything leaves nothing.
    r.subtract({-10, -10, 200, 200});
    LUMEN_CHECK(r.count == 0 && !r.contains(0, 0));
}

LUMEN_TEST(wm_input_region_csd_margin_shape) {
    // A GTK-style window: input everywhere except a 10 px band at the top (a shadow that
    // must not swallow clicks meant for the window behind).
    lx::compositor::input_region r{};
    r.reset_empty();
    r.add({0, 0, 300, 200});
    r.subtract({0, 0, 300, 10});
    LUMEN_CHECK(r.count == 1);
    LUMEN_CHECK(!r.contains(150, 5) && r.contains(150, 10) && r.contains(0, 199));
}

LUMEN_TEST(wm_input_region_extreme_extents_do_not_overflow) {
    // "Everything" as clients spell it: INT32_MAX extents from a negative origin.
    lx::compositor::input_region r{};
    r.reset_empty();
    r.add({-2147483647 - 1, -2147483647 - 1, 2147483647, 2147483647});
    r.add({0, 0, 2147483647, 2147483647});
    LUMEN_CHECK(r.contains(2147483646, 2147483646) && r.contains(-5, -5));
    r.subtract({10, 10, 10, 10});
    LUMEN_CHECK(!r.contains(15, 15) && r.contains(25, 15) && r.contains(2000000000, 5));
}

LUMEN_TEST(wm_input_region_overflow_over_accepts) {
    lx::compositor::input_region r{};
    r.reset_empty();
    // 17 disjoint 1x1 cells on a diagonal: the 17th collapses everything to the bbox.
    for (int i = 0; i < 17; ++i)
        r.add({i * 10, i * 10, 1, 1});
    LUMEN_CHECK(r.approximated && r.count == 1);
    LUMEN_CHECK(r.contains(0, 0) && r.contains(160, 160) && r.contains(5, 100));

    // Subtract that would need more than 16 pieces stops cutting: never under-accepts.
    lx::compositor::input_region s{};
    s.reset_empty();
    for (int i = 0; i < 8; ++i)
        s.add({i * 100, 0, 50, 50});
    s.subtract({-10, 20, 1000, 5}); // splits each of the 8 into 2 → 16, fits
    LUMEN_CHECK(!s.approximated && s.count == 16);
    LUMEN_CHECK(!s.contains(10, 22) && s.contains(10, 19) && s.contains(10, 25));
    s.subtract({20, -10, 5, 100}); // would need more pieces than fit
    LUMEN_CHECK(s.approximated && s.count <= lx::compositor::input_region::k_max);
    // Whatever it could not cut is still accepted; nothing outside the region appeared.
    LUMEN_CHECK(!s.contains(60, 10) && !s.contains(10, 22));
}

// ── interactive grabs ────────────────────────────────────────────────────────

LUMEN_TEST(wm_resize_edges_valid_set_is_the_nine_enum_values) {
    using lx::compositor::valid_resize_edges;
    const unsigned ok[] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
    for (unsigned e : ok)
        LUMEN_CHECK(valid_resize_edges(e));
    const unsigned bad[] = {3, 7, 11, 12, 13, 14, 15, 16, 0xffffffffu};
    for (unsigned e : bad)
        LUMEN_CHECK(!valid_resize_edges(e));
}

LUMEN_TEST(wm_resize_target_moves_only_the_grabbed_edges) {
    using namespace lx::compositor;
    const lx::rect2i start{100, 100, 400, 300};
    const lx::size2i none{};
    // Right/bottom grow with the pointer; left/top grow against it.
    lx::size2i s = resize_target(start, edge_bottom | edge_right, 50.4, 20.6, none, none);
    LUMEN_CHECK(s.width == 450 && s.height == 321);
    s = resize_target(start, edge_top | edge_left, 50, 20, none, none);
    LUMEN_CHECK(s.width == 350 && s.height == 280);
    s = resize_target(start, edge_top | edge_left, -50, -20, none, none);
    LUMEN_CHECK(s.width == 450 && s.height == 320);
    // A side edge leaves the other axis alone however the pointer moves.
    s = resize_target(start, edge_right, 30, 999, none, none);
    LUMEN_CHECK(s.width == 430 && s.height == 300);
    s = resize_target(start, edge_top, 999, -40, none, none);
    LUMEN_CHECK(s.width == 400 && s.height == 340);
}

LUMEN_TEST(wm_resize_target_clamps_to_min_max_and_never_below_one) {
    using namespace lx::compositor;
    const lx::rect2i start{0, 0, 300, 200};
    lx::size2i s = resize_target(start, edge_bottom | edge_right, -400, -400, {100, 100},
                                 {900, 700});
    LUMEN_CHECK(s.width == 100 && s.height == 100);
    s = resize_target(start, edge_bottom | edge_right, 4000, 4000, {100, 100}, {900, 700});
    LUMEN_CHECK(s.width == 900 && s.height == 700);
    // No limits: still at least 1x1.
    s = resize_target(start, edge_top | edge_left, 5000, 5000, {}, {});
    LUMEN_CHECK(s.width == 1 && s.height == 1);
    // Max 0 on an axis means unbounded there.
    s = resize_target(start, edge_right, 10000, 0, {}, {0, 50});
    LUMEN_CHECK(s.width == 10300 && s.height == 200);
    // Absurd deltas saturate instead of overflowing.
    s = resize_target(start, edge_right, 1e300, 0, {}, {});
    LUMEN_CHECK(s.width > 300);
}

LUMEN_TEST(wm_edges_for_point_uses_thirds_and_centre_quadrant) {
    using namespace lx::compositor;
    const lx::rect2i g{100, 100, 300, 300}; // thirds at 200 and 300
    LUMEN_CHECK(edges_for_point(g, 110, 110) == (edge_top | edge_left));
    LUMEN_CHECK(edges_for_point(g, 250, 110) == edge_top);
    LUMEN_CHECK(edges_for_point(g, 390, 110) == (edge_top | edge_right));
    LUMEN_CHECK(edges_for_point(g, 110, 250) == edge_left);
    LUMEN_CHECK(edges_for_point(g, 390, 250) == edge_right);
    LUMEN_CHECK(edges_for_point(g, 110, 390) == (edge_bottom | edge_left));
    LUMEN_CHECK(edges_for_point(g, 250, 390) == edge_bottom);
    LUMEN_CHECK(edges_for_point(g, 390, 390) == (edge_bottom | edge_right));
    // Middle third on both axes: the quadrant's corner.
    LUMEN_CHECK(edges_for_point(g, 240, 240) == (edge_top | edge_left));
    LUMEN_CHECK(edges_for_point(g, 260, 260) == (edge_bottom | edge_right));
    LUMEN_CHECK(edges_for_point(g, 260, 240) == (edge_top | edge_right));
    for (unsigned y = 100; y < 400; y += 7)
        for (unsigned x = 100; x < 400; x += 7)
            LUMEN_CHECK(valid_resize_edges(edges_for_point(g, x, y)) &&
                        edges_for_point(g, x, y) != edge_none);
}

LUMEN_TEST(wm_moved_window_keeps_outstanding_resize_anchor) {
    using namespace lx::compositor;
    // Resize from the left was released; its final configure is still unanswered when the
    // window is moved by (+200, +50). The commit that answers it must keep the right edge
    // where it is *now*, not where the resize began.
    toplevel_record rec{};
    const lx::rect2i start{100, 100, 400, 300}; // right edge at x=500
    rec.geometry = start;
    toplevel_configure c = anchored(edge_left, start);
    c.serial = 7;
    c.size = {450, 300};
    toplevel_manager::on_sent(rec, c, 0);
    rec.desired = c;
    rec.geometry.x += 200;
    rec.geometry.y += 50;
    shift_anchors(rec, 200, 50);
    LUMEN_CHECK(rec.desired.anchor.x == 300 && rec.desired.anchor.y == 150);
    LUMEN_CHECK(toplevel_manager::on_ack(rec, 7) == toplevel_manager::ack_result::ok);
    toplevel_configure applied{};
    LUMEN_CHECK(toplevel_manager::consume_ack(rec, applied));
    const lx::rect2i got = anchored_geometry(rec.geometry, applied, {450, 300});
    LUMEN_CHECK(got.x + got.width == 700); // the moved right edge, not 500
    LUMEN_CHECK(got.x == 250 && got.y == 150);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
