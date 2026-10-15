// wlr-layer-shell arrangement (pure) and the layer shell's configure / map / exclusive-zone /
// keyboard-interactivity state machine. No Wayland: the layer shell runs over a real surface
// tree and surface manager, with recording wire ops and host callbacks.
#include "lumen_test.hpp"

#include <memory>

import lx.foundation;
import lx.runtime;
import lx.gfx;
import lx.scene;
import lx.compositor;

using lx::compositor::arrange_layers;
using lx::compositor::layer_arrange_in;
using lx::compositor::layer_arrange_out;
using lx::compositor::layer_exclusive_edge;
using lx::compositor::layer_props;
using lx::compositor::layer_shell;
using lx::compositor::layer_state_error;
using lx::compositor::validate_layer_state;

namespace {

constexpr unsigned T = lx::compositor::k_layer_anchor_top;
constexpr unsigned B = lx::compositor::k_layer_anchor_bottom;
constexpr unsigned L = lx::compositor::k_layer_anchor_left;
constexpr unsigned R = lx::compositor::k_layer_anchor_right;

constexpr lx::rect2i k_out{0, 0, 1000, 800};

bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

layer_props props(unsigned anchor, int w, int h, int zone = 0) {
    layer_props p{};
    p.anchor = anchor;
    p.size = {w, h};
    p.zone = zone;
    return p;
}

/// One surface alone on the output.
layer_arrange_out arrange_one(const layer_props& p, lx::size2i committed = {}, bool mapped = false,
                              lx::rect2i* usable_out = nullptr) {
    layer_arrange_in in{&p, committed, mapped, true};
    layer_arrange_out out{};
    lx::rect2i usable{};
    arrange_layers(k_out, &in, &out, 1, usable);
    if (usable_out)
        *usable_out = usable;
    return out;
}

} // namespace

// ── arrangement ──────────────────────────────────────────────────────────────

LUMEN_TEST(layer_every_anchor_combination_positions_a_fixed_size_surface) {
    // 100x50 on 1000x800. Per axis: low anchor → 0, high → end - size, both or neither →
    // centred.
    for (unsigned a = 0; a < 16; ++a) {
        const auto o = arrange_one(props(a, 100, 50));
        const bool l = a & L, r = a & R, t = a & T, b = a & B;
        const int x = (l && !r) ? 0 : (r && !l) ? 900 : 450;
        const int y = (t && !b) ? 0 : (b && !t) ? 750 : 375;
        LUMEN_CHECK(!o.closed);
        LUMEN_CHECK(same(o.box, {x, y, 100, 50}));
        LUMEN_CHECK(o.configure.width == 100 && o.configure.height == 50);
    }
}

LUMEN_TEST(layer_zero_size_takes_the_span_between_both_anchors) {
    const auto bar = arrange_one(props(T | L | R, 0, 32));
    LUMEN_CHECK(same(bar.box, {0, 0, 1000, 32}));
    LUMEN_CHECK(bar.configure.width == 1000 && bar.configure.height == 32);
    const auto bg = arrange_one(props(T | B | L | R, 0, 0, -1));
    LUMEN_CHECK(same(bg.box, k_out));
    LUMEN_CHECK(bg.configure.width == 1000 && bg.configure.height == 800);
}

LUMEN_TEST(layer_margins_apply_only_on_anchored_edges) {
    auto p = props(B | R, 100, 80);
    p.margin_top = 7;
    p.margin_right = 10;
    p.margin_bottom = 20;
    p.margin_left = 9;
    LUMEN_CHECK(same(arrange_one(p).box, {1000 - 110, 800 - 100, 100, 80}));
    // Not anchored at all: margins have no effect, centred.
    p.anchor = 0;
    LUMEN_CHECK(same(arrange_one(p).box, {450, 360, 100, 80}));
    // Both horizontal anchors with a zero width: the span between the margins.
    auto bar = props(T | L | R, 0, 30);
    bar.margin_left = 5;
    bar.margin_right = 15;
    bar.margin_top = 3;
    const auto o = arrange_one(bar);
    LUMEN_CHECK(same(o.box, {5, 3, 980, 30}));
    LUMEN_CHECK(o.configure.width == 980);
}

LUMEN_TEST(layer_smaller_committed_size_is_centred_between_opposite_anchors) {
    // Configured 1000 wide, committed 600: centred (the spec's text), not left-aligned.
    const auto o = arrange_one(props(T | L | R, 0, 32), {600, 32}, true);
    LUMEN_CHECK(same(o.box, {200, 0, 600, 32}));
    LUMEN_CHECK(o.configure.width == 1000);
    // One anchor: placed at that edge with the committed size.
    const auto r = arrange_one(props(R, 200, 100), {150, 90}, true);
    LUMEN_CHECK(same(r.box, {850, 355, 150, 90}));
}

LUMEN_TEST(layer_exclusive_zone_counts_only_once_mapped) {
    lx::rect2i usable{};
    (void)arrange_one(props(T | L | R, 0, 32, 32), {1000, 32}, false, &usable);
    LUMEN_CHECK(same(usable, k_out));
    const auto o = arrange_one(props(T | L | R, 0, 32, 32), {1000, 32}, true, &usable);
    LUMEN_CHECK(same(usable, {0, 32, 1000, 768}));
    LUMEN_CHECK(o.zone_edge == T);
}

LUMEN_TEST(layer_exclusive_zone_includes_the_margin_per_edge) {
    struct row {
        unsigned anchor;
        lx::rect2i usable;
    } rows[] = {
        {T, {0, 25, 1000, 775}},
        {B, {0, 0, 1000, 775}},
        {L, {25, 0, 975, 800}},
        {R, {0, 0, 975, 800}},
    };
    for (const auto& r : rows) {
        auto p = props(r.anchor, 40, 40, 20);
        p.margin_top = p.margin_bottom = p.margin_left = p.margin_right = 5;
        lx::rect2i usable{};
        (void)arrange_one(p, {40, 40}, true, &usable);
        LUMEN_CHECK(same(usable, r.usable));
    }
}

LUMEN_TEST(layer_exclusive_edge_deduction) {
    // Single edge, edge + both perpendicular: that edge.
    LUMEN_CHECK(layer_exclusive_edge(props(T, 0, 0, 10)) == T);
    LUMEN_CHECK(layer_exclusive_edge(props(T | L | R, 0, 0, 10)) == T);
    LUMEN_CHECK(layer_exclusive_edge(props(B | L | R, 0, 0, 10)) == B);
    LUMEN_CHECK(layer_exclusive_edge(props(L | T | B, 0, 0, 10)) == L);
    LUMEN_CHECK(layer_exclusive_edge(props(R | T | B, 0, 0, 10)) == R);
    // Corner, parallel pair, all four, none: no zone.
    LUMEN_CHECK(layer_exclusive_edge(props(T | L, 0, 0, 10)) == 0);
    LUMEN_CHECK(layer_exclusive_edge(props(L | R, 0, 0, 10)) == 0);
    LUMEN_CHECK(layer_exclusive_edge(props(T | B | L | R, 0, 0, 10)) == 0);
    LUMEN_CHECK(layer_exclusive_edge(props(0, 0, 0, 10)) == 0);
    // A zone ≤ 0 never has an edge.
    LUMEN_CHECK(layer_exclusive_edge(props(T, 0, 0, 0)) == 0);
    LUMEN_CHECK(layer_exclusive_edge(props(T, 0, 0, -1)) == 0);
    // v5 set_exclusive_edge disambiguates a corner.
    auto corner = props(T | L, 0, 0, 10);
    corner.exclusive_edge = L;
    LUMEN_CHECK(layer_exclusive_edge(corner) == L);
    lx::rect2i usable{};
    corner.size = {50, 50};
    (void)arrange_one(corner, {50, 50}, true, &usable);
    LUMEN_CHECK(same(usable, {10, 0, 990, 800}));
}

LUMEN_TEST(layer_zone_minus_one_uses_the_full_output_zone_zero_the_usable_area) {
    const layer_props panel = props(T | L | R, 0, 30, 30);
    const layer_props full = props(T | B | L | R, 0, 0, -1);
    const layer_props respect = props(T | B | L | R, 0, 0, 0);
    layer_arrange_in in[3] = {{&panel, {1000, 30}, true, true},
                              {&full, {}, false, true},
                              {&respect, {}, false, true}};
    layer_arrange_out out[3]{};
    lx::rect2i usable{};
    arrange_layers(k_out, in, out, 3, usable);
    LUMEN_CHECK(same(usable, {0, 30, 1000, 770}));
    LUMEN_CHECK(same(out[1].box, k_out));
    LUMEN_CHECK(same(out[2].box, {0, 30, 1000, 770}));
}

LUMEN_TEST(layer_exclusive_surfaces_arrange_first_in_layer_order) {
    // A non-exclusive overlay listed first still sees the exclusive bottom-layer panel's
    // zone: the two passes put every exclusive surface ahead of every other one.
    const layer_props overlay = props(T | L | R, 0, 50, 0);
    const layer_props panel = props(T | L | R, 0, 40, 40);
    const layer_props dock = props(B | L | R, 0, 60, 60);
    layer_arrange_in in[3] = {{&overlay, {}, false, true},
                              {&panel, {1000, 40}, true, true},
                              {&dock, {1000, 60}, true, true}};
    layer_arrange_out out[3]{};
    lx::rect2i usable{};
    arrange_layers(k_out, in, out, 3, usable);
    LUMEN_CHECK(same(out[1].box, {0, 0, 1000, 40}));
    LUMEN_CHECK(same(out[2].box, {0, 740, 1000, 60}));
    LUMEN_CHECK(same(out[0].box, {0, 40, 1000, 50}));
    LUMEN_CHECK(same(usable, {0, 40, 1000, 700}));
    // Two exclusive panels on one edge stack, in order.
    const layer_props second = props(T | L | R, 0, 20, 20);
    layer_arrange_in in2[2] = {{&panel, {1000, 40}, true, true}, {&second, {1000, 20}, true, true}};
    layer_arrange_out out2[2]{};
    arrange_layers(k_out, in2, out2, 2, usable);
    LUMEN_CHECK(same(out2[1].box, {0, 40, 1000, 20}));
    LUMEN_CHECK(same(usable, {0, 60, 1000, 740}));
}

LUMEN_TEST(layer_usable_area_is_clamped_and_a_non_positive_assigned_size_closes) {
    const layer_props huge = props(L | T | B, 50, 0, 2000);
    const layer_props filler = props(T | B | L | R, 0, 0, 0);
    layer_arrange_in in[2] = {{&huge, {50, 800}, true, true}, {&filler, {}, false, true}};
    layer_arrange_out out[2]{};
    lx::rect2i usable{};
    arrange_layers(k_out, in, out, 2, usable);
    LUMEN_CHECK(usable.width == 0 && usable.height == 800);
    LUMEN_CHECK(out[1].closed);
    LUMEN_CHECK(!out[0].closed);
    // Margins larger than the output on a stretched axis close it too.
    auto squeezed = props(L | R, 0, 20);
    squeezed.margin_left = 600;
    squeezed.margin_right = 600;
    LUMEN_CHECK(arrange_one(squeezed).closed);
}

LUMEN_TEST(layer_non_participants_are_skipped) {
    const layer_props panel = props(T | L | R, 0, 30, 30);
    layer_arrange_in in{&panel, {1000, 30}, true, false};
    layer_arrange_out out{};
    lx::rect2i usable{};
    arrange_layers(k_out, &in, &out, 1, usable);
    LUMEN_CHECK(same(usable, k_out));
    LUMEN_CHECK(same(out.box, {}));
}

LUMEN_TEST(layer_commit_rules) {
    LUMEN_CHECK(validate_layer_state(props(T | L | R, 0, 32)) == layer_state_error::none);
    LUMEN_CHECK(validate_layer_state(props(T, 0, 32)) == layer_state_error::invalid_size);
    LUMEN_CHECK(validate_layer_state(props(L | R, 100, 0)) == layer_state_error::invalid_size);
    LUMEN_CHECK(validate_layer_state(props(T | B, 100, 0)) == layer_state_error::none);
    auto p = props(T | L, 10, 10, 5);
    p.exclusive_edge = B;
    LUMEN_CHECK(validate_layer_state(p) == layer_state_error::invalid_exclusive_edge);
    p.exclusive_edge = L;
    LUMEN_CHECK(validate_layer_state(p) == layer_state_error::none);
}

// ── layer_shell state machine ────────────────────────────────────────────────

namespace {

struct wire_log {
    unsigned configures = 0;
    unsigned last_serial = 0;
    int last_w = 0;
    int last_h = 0;
    void* last_res = nullptr;
    unsigned closed = 0;
    unsigned serial = 100;
    unsigned usable_changes = 0;
    lx::rect2i usable{};
    unsigned focus_changes = 0;
    unsigned moved = 0;
    unsigned unmapped = 0;
};
wire_log* g_wire = nullptr;

struct fixture {
    lx::compositor::surface_manager surfaces{};
    lx::scene::scene_graph scene{};
    lx::scene::container_node levels[4]{};
    lx::compositor::surface_tree tree{};
    layer_shell layers{};
    wire_log wire{};
    unsigned long long next_buffer = 1;
    int res[64]{};

    fixture() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        g_wire = &wire;
        surfaces.set_scene(&scene);
        tree.bind(surfaces, scene);
        for (auto& c : levels)
            (void)scene.root().add(&c);
        lx::scene::container_node* const ptrs[4] = {&levels[0], &levels[1], &levels[2], &levels[3]};
        layers.bind(surfaces, tree, ptrs);
        lx::compositor::layer_ops ops{};
        ops.send_configure = [](void* r, unsigned serial, int w, int h) {
            ++g_wire->configures;
            g_wire->last_serial = serial;
            g_wire->last_w = w;
            g_wire->last_h = h;
            g_wire->last_res = r;
        };
        ops.send_closed = [](void*) { ++g_wire->closed; };
        ops.next_serial = [](void*) { return ++g_wire->serial; };
        ops.client_alive = [](void*) { return true; };
        layers.set_ops(ops);
        lx::compositor::layer_host host{};
        host.output_area = [](void*) { return k_out; };
        host.usable_changed = [](void*, lx::rect2i u) {
            ++g_wire->usable_changes;
            g_wire->usable = u;
        };
        host.focus_changed = [](void*) { ++g_wire->focus_changes; };
        host.surface_moved = [](void*, lx::surface_id) { ++g_wire->moved; };
        host.surface_unmapped = [](void*, lx::surface_id, const char*) { ++g_wire->unmapped; };
        layers.set_host(host);
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
        LUMEN_CHECK(static_cast<bool>(
            surfaces.on_commit(static_cast<lx::compositor::surface_commit&&>(c))));
        tree.set_has_buffer(s, true);
        tree.mark_dirty(s);
    }

    /// get_layer_surface + props + initial commit.
    lx::surface_id create(unsigned n, unsigned level, const layer_props& p) {
        const lx::surface_id s = make(n);
        LUMEN_CHECK(layers.create(s, lx::client_id{1}, &res[0], &res[n % 64], &res[n % 64], 5,
                                  level, "test") == layer_shell::create_error::none);
        layers.find(s)->pending = p;
        layers.find(s)->pending.layer = level;
        layers.apply_pending(s);
        layers.on_commit(s, {});
        return s;
    }

    /// ack the last configure and commit content of size w x h (the configured size when 0).
    void map(lx::surface_id s, int w = 0, int h = 0) {
        LUMEN_CHECK(layers.on_ack(s, wire.last_serial) == layer_shell::ack_result::ok);
        const int cw = w ? w : wire.last_w;
        const int ch = h ? h : wire.last_h;
        content(s, cw, ch);
        lx::compositor::layer_commit_info info{};
        info.has_buffer = true;
        info.size = {cw, ch};
        layers.on_commit(s, info);
        tree.sync_scene(s, "commit");
    }
};

std::unique_ptr<fixture> make_fixture() { return std::make_unique<fixture>(); }

} // namespace

LUMEN_TEST(layer_initial_commit_configures_and_ack_commit_maps_at_its_box) {
    auto f = make_fixture();
    const auto s = f->create(1, 2, props(T | L | R, 0, 32, 32));
    LUMEN_CHECK(f->wire.configures == 1);
    LUMEN_CHECK(f->wire.last_w == 1000 && f->wire.last_h == 32);
    // Not mapped: the zone does not count yet.
    LUMEN_CHECK(f->layers.arranged() && same(f->layers.usable(), k_out));
    f->map(s);
    const auto* r = f->layers.find(s);
    LUMEN_CHECK(r && r->mapped);
    LUMEN_CHECK(same(r->box, {0, 0, 1000, 32}));
    LUMEN_CHECK(same(f->surfaces.node_rect(s), {0, 0, 1000, 32}));
    LUMEN_CHECK(same(f->layers.usable(), {0, 32, 1000, 768}));
    LUMEN_CHECK(same(f->wire.usable, {0, 32, 1000, 768}));
    LUMEN_CHECK(f->levels[2].child_count() == 1);
    // Mapping the same size again sends no new configure.
    LUMEN_CHECK(f->wire.configures == 1);
}

LUMEN_TEST(layer_ack_rules) {
    auto f = make_fixture();
    const auto s = f->create(1, 2, props(T | L | R, 0, 32));
    const unsigned s1 = f->wire.last_serial;
    LUMEN_CHECK(f->layers.on_ack(s, s1 + 9) == layer_shell::ack_result::invalid);
    LUMEN_CHECK(f->layers.on_ack(s, s1) == layer_shell::ack_result::ok);
    LUMEN_CHECK(f->layers.on_ack(s, s1) == layer_shell::ack_result::duplicate);
}

LUMEN_TEST(layer_second_panel_is_pushed_and_moved_with_the_first) {
    auto f = make_fixture();
    const auto a = f->create(1, 2, props(T | L | R, 0, 30, 30));
    f->map(a);
    const auto b = f->create(2, 2, props(T | L | R, 0, 20, 20));
    f->map(b);
    LUMEN_CHECK(same(f->layers.find(b)->box, {0, 30, 1000, 20}));
    LUMEN_CHECK(same(f->layers.usable(), {0, 50, 1000, 750}));
    // The first panel unmaps (null buffer): the second moves up, in the same call.
    const unsigned moved = f->wire.moved;
    f->layers.on_commit(a, {});
    LUMEN_CHECK(!f->layers.find(a)->mapped && !f->layers.find(a)->initial_commit_seen);
    LUMEN_CHECK(same(f->layers.find(b)->box, {0, 0, 1000, 20}));
    LUMEN_CHECK(same(f->surfaces.node_rect(b), {0, 0, 1000, 20}));
    LUMEN_CHECK(f->wire.moved == moved + 1);
    LUMEN_CHECK(same(f->layers.usable(), {0, 20, 1000, 780}));
}

LUMEN_TEST(layer_destroy_releases_the_zone) {
    auto f = make_fixture();
    const auto a = f->create(1, 1, props(L | T | B, 64, 0, 64));
    f->map(a);
    LUMEN_CHECK(same(f->layers.usable(), {64, 0, 936, 800}));
    f->layers.destroy(a);
    LUMEN_CHECK(!f->layers.find(a));
    LUMEN_CHECK(same(f->layers.usable(), k_out));
    LUMEN_CHECK(f->levels[1].child_count() == 0);
}

LUMEN_TEST(layer_set_layer_moves_the_tree_between_containers) {
    auto f = make_fixture();
    const auto a = f->create(1, 0, props(T | B | L | R, 0, 0, -1));
    f->map(a);
    LUMEN_CHECK(f->levels[0].child_count() == 1);
    f->layers.find(a)->pending.layer = 3;
    f->layers.apply_pending(a);
    lx::compositor::layer_commit_info info{};
    info.has_buffer = true;
    info.size = {1000, 800};
    f->layers.on_commit(a, info);
    LUMEN_CHECK(f->levels[0].child_count() == 0 && f->levels[3].child_count() == 1);
    lx::compositor::tree_hit th{};
    const lx::compositor::layer_record* rec = nullptr;
    LUMEN_CHECK(f->layers.hit(lx::compositor::layer_level::overlay, 5, 5, th, rec) && rec &&
                rec->surface == a);
    LUMEN_CHECK(!f->layers.hit(lx::compositor::layer_level::background, 5, 5, th, rec));
}

LUMEN_TEST(layer_exclusive_focus_is_the_newest_on_the_highest_layer) {
    auto f = make_fixture();
    auto k = props(0, 100, 100);
    k.interactivity = 1; // exclusive
    const auto bottom = f->create(1, 1, k);
    f->map(bottom);
    LUMEN_CHECK(f->layers.exclusive_focus() == nullptr); // bottom: normal semantics
    LUMEN_CHECK(f->layers.click_focusable(bottom));
    const auto top = f->create(2, 2, k);
    f->map(top);
    const auto overlay = f->create(3, 3, k);
    f->map(overlay);
    LUMEN_CHECK(f->layers.exclusive_focus() && f->layers.exclusive_focus()->surface == overlay);
    f->layers.on_commit(overlay, {}); // unmapped
    LUMEN_CHECK(f->layers.exclusive_focus() && f->layers.exclusive_focus()->surface == top);
    auto none = props(0, 10, 10);
    const auto quiet = f->create(4, 3, none);
    f->map(quiet);
    LUMEN_CHECK(!f->layers.focusable(quiet) && !f->layers.click_focusable(quiet));
    auto od = props(0, 10, 10);
    od.interactivity = 2;
    const auto on_demand = f->create(5, 2, od);
    f->map(on_demand);
    LUMEN_CHECK(f->layers.focusable(on_demand) && f->layers.click_focusable(on_demand));
    LUMEN_CHECK(!f->layers.click_focusable(top)); // exclusive above the windows: not by click
}

LUMEN_TEST(layer_closed_when_no_room_is_left_and_inert_afterwards) {
    auto f = make_fixture();
    const auto wall = f->create(1, 2, props(L | T | B, 50, 0, 1000));
    f->map(wall);
    LUMEN_CHECK(f->layers.usable().width == 0);
    const auto fill = f->create(2, 2, props(T | B | L | R, 0, 0, 0));
    LUMEN_CHECK(f->wire.closed == 1);
    const auto* r = f->layers.find(fill);
    LUMEN_CHECK(r && r->closed && !f->layers.drawable(fill));
    const unsigned configures = f->wire.configures;
    f->layers.on_commit(fill, {});
    LUMEN_CHECK(f->wire.configures == configures);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
