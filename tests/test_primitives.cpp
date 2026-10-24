#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.layout;
import lx.gfx;
import lx.scene;
import lx.ui;

// ADR-010 Phase 0: `lx.gfx:primitives`, `lx.scene::draw_list` v3, `lx.ui::draw_scope`'s
// emitters and `ui_root::tick`'s idle rule.

namespace {

using lx::gfx::primitive_kind;

[[nodiscard]] float half_round_trip(float v) {
    return lx::gfx::from_half(lx::gfx::to_half(v));
}

[[nodiscard]] lx::gfx::quad_instance quad_at(int x, int y) {
    lx::gfx::quad_instance q{};
    q.bounds[0] = static_cast<float>(x);
    q.bounds[1] = static_cast<float>(y);
    q.bounds[2] = 10.f;
    q.bounds[3] = 10.f;
    return q;
}

[[nodiscard]] lx::scene::draw_command surface_at(unsigned texture, int x, unsigned z) {
    lx::scene::draw_command cmd{};
    cmd.texture = lx::texture_id{texture};
    cmd.dst = {x, 0, 10, 10};
    cmd.sort_key = lx::draw_sort_key::make(0, z);
    return cmd;
}

} // namespace

// ── lx.gfx:primitives ───────────────────────────────────────────────────────────

LUMEN_TEST(instance_layouts_match_adr_010) {
    static_assert(sizeof(lx::gfx::quad_instance) == 80);
    static_assert(sizeof(lx::gfx::shadow_instance) == 64);
    static_assert(sizeof(lx::gfx::sprite_instance) == 64);
    static_assert(sizeof(lx::gfx::underline_instance) == 48);
    static_assert(sizeof(lx::gfx::primitive_ref) == 12);
    static_assert(alignof(lx::gfx::quad_instance) == 4 && alignof(lx::gfx::shadow_instance) == 4 &&
                  alignof(lx::gfx::sprite_instance) == 4 &&
                  alignof(lx::gfx::underline_instance) == 4);
    static_assert(std::is_trivially_copyable_v<lx::gfx::quad_instance> &&
                  std::is_standard_layout_v<lx::gfx::quad_instance>);
    static_assert(std::is_trivially_copyable_v<lx::gfx::sprite_instance> &&
                  std::is_standard_layout_v<lx::gfx::sprite_instance>);
    // The declaration order is the tie-break within one order: shadows under quads, the
    // compositor's surfaces last.
    static_assert(static_cast<int>(primitive_kind::shadow) < static_cast<int>(primitive_kind::quad));
    static_assert(static_cast<int>(primitive_kind::quad) < static_cast<int>(primitive_kind::path));
    static_assert(static_cast<int>(primitive_kind::path) <
                  static_cast<int>(primitive_kind::underline));
    static_assert(static_cast<int>(primitive_kind::underline) <
                  static_cast<int>(primitive_kind::mono_sprite));
    static_assert(static_cast<int>(primitive_kind::mono_sprite) <
                  static_cast<int>(primitive_kind::poly_sprite));
    static_assert(static_cast<int>(primitive_kind::poly_sprite) <
                  static_cast<int>(primitive_kind::surface));
    LUMEN_CHECK(lx::gfx::k_max_primitives == 16384);
    LUMEN_CHECK(lx::gfx::k_max_batches == 1024);
    LUMEN_CHECK(lx::gfx::k_order_cell_px == 64);
    LUMEN_CHECK(lx::gfx::k_max_order_cells == 16384);
}

LUMEN_TEST(half_round_trips_representable_values) {
    static_assert(lx::gfx::to_half(0.f) == 0x0000);
    static_assert(lx::gfx::to_half(1.f) == 0x3C00);
    static_assert(lx::gfx::to_half(-2.5f) == 0xC100);
    static_assert(lx::gfx::to_half(65504.f) == 0x7BFF);
    for (const float v : {0.f, 1.f, 0.5f, 65504.f, -2.5f, 0.25f, 1024.f, 0.1240234375f})
        LUMEN_CHECK(half_round_trip(v) == v);
    LUMEN_CHECK(std::signbit(half_round_trip(-0.f)));
    // A float a half cannot hold rounds to the nearest half, ties to even: 1 + 2^-11 is
    // exactly halfway between 1 and the next half (1 + 2^-10) and lands on the even one.
    LUMEN_CHECK(lx::gfx::to_half(1.f + 0.00048828125f) == 0x3C00);
    LUMEN_CHECK(lx::gfx::to_half(1.f + 3.f * 0.00048828125f) == 0x3C02);
    LUMEN_CHECK(lx::gfx::to_half(1.f + 0.0005f) == 0x3C01);
}

LUMEN_TEST(half_handles_zero_denormals_overflow_and_nan) {
    // Below half the smallest subnormal (2^-25) is zero.
    LUMEN_CHECK(lx::gfx::to_half(1e-8f) == 0);
    LUMEN_CHECK(half_round_trip(1e-8f) == 0.f);
    // Subnormals: 2^-24 is the smallest half, 2^-25 a tie that rounds to even (zero),
    // anything past the tie rounds up to it.
    LUMEN_CHECK(lx::gfx::to_half(0x1p-24f) == 0x0001);
    LUMEN_CHECK(lx::gfx::from_half(0x0001) == 0x1p-24f);
    LUMEN_CHECK(lx::gfx::to_half(0x1p-25f) == 0x0000);
    LUMEN_CHECK(lx::gfx::to_half(1.5f * 0x1p-25f) == 0x0001);
    LUMEN_CHECK(half_round_trip(0x1p-14f) == 0x1p-14f); // smallest normal
    LUMEN_CHECK(half_round_trip(0x1p-15f) == 0x1p-15f); // largest subnormal power
    // Finite input never becomes infinity: it clamps to the largest finite half.
    LUMEN_CHECK(lx::gfx::to_half(1e6f) == 0x7BFF);
    LUMEN_CHECK(lx::gfx::to_half(-1e6f) == 0xFBFF);
    LUMEN_CHECK(lx::gfx::to_half(65520.f) == 0x7BFF);
    LUMEN_CHECK(lx::gfx::from_half(0x7BFF) == 65504.f);
    LUMEN_CHECK(lx::gfx::to_half(std::numeric_limits<float>::infinity()) == 0x7C00);
    LUMEN_CHECK(std::isinf(lx::gfx::from_half(0x7C00)));
    LUMEN_CHECK(std::isnan(half_round_trip(std::numeric_limits<float>::quiet_NaN())));
}

LUMEN_TEST(premultiplied_half_premultiplies_once_and_ignores_space) {
    const lx::gfx::packed_rgba p = lx::gfx::premultiplied_half(lx::color::rgb(1.f, 0.5f, 0.f, 0.5f));
    LUMEN_CHECK(p.r == lx::gfx::to_half(0.5f));
    LUMEN_CHECK(p.g == lx::gfx::to_half(0.25f));
    LUMEN_CHECK(p.b == 0);
    LUMEN_CHECK(p.a == lx::gfx::to_half(0.5f));
    // No colour-space conversion: the caller supplies the blend space's values.
    const lx::gfx::packed_rgba linear = lx::gfx::premultiplied_half(lx::color::linear(1.f, 0.5f, 0.f, 0.5f));
    LUMEN_CHECK(linear.r == p.r && linear.g == p.g && linear.b == p.b && linear.a == p.a);
    const lx::gfx::packed_rgba opaque = lx::gfx::premultiplied_half(lx::color::rgb(0.25f, 1.f, 1.f));
    LUMEN_CHECK(opaque.r == lx::gfx::to_half(0.25f) && opaque.a == lx::gfx::to_half(1.f));
}

LUMEN_TEST(order_grid_shares_orders_until_rects_overlap) {
    lx::gfx::order_grid g{};
    g.reset({640, 480});
    LUMEN_CHECK(g.cell_px() == 64);
    LUMEN_CHECK(g.cells_x() == 10 && g.cells_y() == 8);
    const auto a = g.assign({0, 0, 64, 64});
    const auto b = g.assign({128, 128, 64, 64});
    LUMEN_CHECK(a == 1);
    LUMEN_CHECK(b == 1); // no overlap with `a`: same order, one batch
    const auto c = g.assign({32, 32, 64, 64});
    LUMEN_CHECK(c == 2); // overlaps `a`
    const auto d = g.assign({0, 0, 640, 480});
    LUMEN_CHECK(d == 3); // covers everything
    LUMEN_CHECK(g.max_order() == 3);
    LUMEN_CHECK(g.assign({600, 400, 10, 10}) == 4);
}

LUMEN_TEST(order_grid_push_layer_lifts_the_base) {
    lx::gfx::order_grid g{};
    g.reset({640, 480});
    LUMEN_CHECK(g.assign({0, 0, 10, 10}) == 1);
    g.push_layer();
    LUMEN_CHECK(g.layer_base() == 2);
    LUMEN_CHECK(g.layer_depth() == 1);
    // Does not overlap anything, yet sorts above everything drawn before the layer.
    LUMEN_CHECK(g.assign({500, 400, 10, 10}) == 2);
    LUMEN_CHECK(g.assign({500, 400, 10, 10}) == 3);
    g.pop_layer();
    LUMEN_CHECK(g.layer_base() == 0);
    LUMEN_CHECK(g.assign({600, 0, 10, 10}) == 1);
    // The stack is fixed: past 32 deep the innermost base is kept.
    for (unsigned i = 0; i < 40; ++i)
        g.push_layer();
    LUMEN_CHECK(g.layer_depth() == lx::gfx::k_max_order_layers);
    for (unsigned i = 0; i < 40; ++i)
        g.pop_layer();
    LUMEN_CHECK(g.layer_depth() == 0);
    LUMEN_CHECK(g.layer_base() == 0);
}

LUMEN_TEST(order_grid_off_view_and_empty_rects_take_the_base) {
    lx::gfx::order_grid g{};
    g.reset({640, 480});
    LUMEN_CHECK(g.assign({700, 0, 10, 10}) == 0);
    LUMEN_CHECK(g.assign({0, 480, 10, 10}) == 0);
    LUMEN_CHECK(g.assign({0, 0, 0, 0}) == 0);
    LUMEN_CHECK(g.assign({10, 10, -5, 5}) == 0);
    LUMEN_CHECK(g.max_order() == 0);
    // Partly off-view: clamped to the view, so it still takes a cell.
    LUMEN_CHECK(g.assign({-5, -5, 10, 10}) == 1);
    LUMEN_CHECK(g.assign({635, 475, 100, 100}) == 1);
    g.push_layer();
    LUMEN_CHECK(g.assign({700, 0, 10, 10}) == g.layer_base());
    // Reset clears what was assigned.
    g.reset({640, 480});
    LUMEN_CHECK(g.max_order() == 0);
    LUMEN_CHECK(g.layer_depth() == 0);
    LUMEN_CHECK(g.assign({0, 0, 10, 10}) == 1);
}

LUMEN_TEST(order_grid_doubles_its_cell_past_the_cell_cap) {
    lx::gfx::order_grid g{};
    g.reset({8192, 8192});
    LUMEN_CHECK(g.cell_px() == 64);
    LUMEN_CHECK(g.cells_x() * g.cells_y() == lx::gfx::k_max_order_cells);
    g.reset({8192, 8256});
    LUMEN_CHECK(g.cell_px() == 128);
    LUMEN_CHECK(g.cells_x() == 64 && g.cells_y() == 65);
    g.reset({16384, 16384});
    LUMEN_CHECK(g.cell_px() == 128);
    g.reset({16385, 16384});
    LUMEN_CHECK(g.cell_px() == 256);
    // Orders still work at the coarser cell.
    LUMEN_CHECK(g.assign({0, 0, 10, 10}) == 1);
    LUMEN_CHECK(g.assign({250, 250, 5, 5}) == 2);   // same 256 px cell
    LUMEN_CHECK(g.assign({256, 256, 10, 10}) == 1); // the next cell over
    LUMEN_CHECK(g.assign({250, 250, 10, 10}) == 3); // straddles both
    // Empty: unbounded, 8192 × 8192 at 64 px.
    g.reset({});
    LUMEN_CHECK(g.cell_px() == 64);
    LUMEN_CHECK(g.view().width == 8192 && g.view().height == 8192);
    LUMEN_CHECK(g.assign({8192, 0, 10, 10}) == 0);
    LUMEN_CHECK(g.assign({8000, 8000, 10, 10}) == 1);
}

// ── lx.scene::draw_list v3 ──────────────────────────────────────────────────────

LUMEN_TEST(draw_list_cuts_batches_on_kind_and_page) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    // Everything at one order: the sort falls through to kind, then page, then push order.
    lx::gfx::sprite_instance sprite{};
    lx::gfx::shadow_instance shadow{};
    lx::gfx::underline_instance underline{};
    list.push_sprite(primitive_kind::mono_sprite, 1, sprite, {40, 0, 10, 10}, 1);
    list.push_quad(quad_at(0, 0), {0, 0, 10, 10}, 1);
    list.push_underline(underline, {50, 0, 10, 10}, 1);
    list.push_shadow(shadow, {20, 0, 10, 10}, 1);
    list.push_sprite(primitive_kind::mono_sprite, 0, sprite, {30, 0, 10, 10}, 1);
    list.push_quad(quad_at(10, 0), {10, 0, 10, 10}, 1);
    list.push_sprite(primitive_kind::poly_sprite, 0, sprite, {60, 0, 10, 10}, 1);
    list.push(surface_at(1, 70, 0), 1u);
    LUMEN_CHECK(list.primitive_count() == 7);
    LUMEN_CHECK(list.quad_instances().size() == 2);
    LUMEN_CHECK(list.shadow_instances().size() == 1);
    LUMEN_CHECK(list.sprite_instances().size() == 3);
    LUMEN_CHECK(list.underline_instances().size() == 1);
    LUMEN_CHECK(list.refs().empty()); // sorted only by batch_for_render

    list.batch_for_render();
    const auto refs = list.refs();
    LUMEN_CHECK(refs.size() == 8);
    const primitive_kind expected[] = {primitive_kind::shadow,      primitive_kind::quad,
                                       primitive_kind::quad,        primitive_kind::underline,
                                       primitive_kind::mono_sprite, primitive_kind::mono_sprite,
                                       primitive_kind::poly_sprite, primitive_kind::surface};
    for (unsigned i = 0; i < 8; ++i)
        LUMEN_CHECK(refs[i].kind == expected[i]);
    LUMEN_CHECK(refs[4].page == 0 && refs[5].page == 1); // page sorts within a kind
    LUMEN_CHECK(refs[1].index == 0 && refs[2].index == 1); // quads keep push order
    LUMEN_CHECK(refs[7].index == 0); // the surface indexes the draw_command array

    const auto batches = list.batches();
    LUMEN_CHECK(batches.size() == 7); // the two quads share one; the two pages do not
    LUMEN_CHECK(batches[0].kind == primitive_kind::shadow && batches[0].count == 1);
    LUMEN_CHECK(batches[1].kind == primitive_kind::quad && batches[1].first == 1 &&
                batches[1].count == 2);
    LUMEN_CHECK(batches[1].bounds.x == 0 && batches[1].bounds.width == 20 &&
                batches[1].bounds.height == 10);
    LUMEN_CHECK(batches[3].kind == primitive_kind::mono_sprite && batches[3].page == 0);
    LUMEN_CHECK(batches[4].kind == primitive_kind::mono_sprite && batches[4].page == 1);
    LUMEN_CHECK(batches[6].kind == primitive_kind::surface && batches[6].first == 7);
    LUMEN_CHECK(list.overflow_drops() == 0);
}

LUMEN_TEST(draw_list_sorts_by_order_then_stably) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    list.push_quad(quad_at(0, 0), {0, 0, 10, 10}, 3);
    list.push_quad(quad_at(10, 0), {10, 0, 10, 10}, 1);
    list.push_quad(quad_at(20, 0), {20, 0, 10, 10}, 2);
    list.push_quad(quad_at(30, 0), {30, 0, 10, 10}, 1);
    list.push_quad(quad_at(40, 0), {40, 0, 10, 10}, 1);
    lx::gfx::shadow_instance shadow{};
    list.push_shadow(shadow, {50, 0, 10, 10}, 2); // same order as quad 2: shadow first
    list.batch_for_render();
    const auto refs = list.refs();
    LUMEN_CHECK(refs.size() == 6);
    // Order 1 in push order, then the shadow before the quad at order 2, then order 3.
    LUMEN_CHECK(refs[0].order == 1 && refs[0].index == 1);
    LUMEN_CHECK(refs[1].order == 1 && refs[1].index == 3);
    LUMEN_CHECK(refs[2].order == 1 && refs[2].index == 4);
    LUMEN_CHECK(refs[3].order == 2 && refs[3].kind == primitive_kind::shadow);
    LUMEN_CHECK(refs[4].order == 2 && refs[4].kind == primitive_kind::quad && refs[4].index == 2);
    LUMEN_CHECK(refs[5].order == 3 && refs[5].index == 0);
    const auto batches = list.batches();
    LUMEN_CHECK(batches.size() == 3);
    LUMEN_CHECK(batches[0].count == 3 && batches[1].kind == primitive_kind::shadow &&
                batches[2].count == 2);
    // Batching again gives the same answer: push order is never disturbed.
    list.batch_for_render();
    LUMEN_CHECK(list.refs().size() == 6);
    LUMEN_CHECK(list.refs()[5].index == 0 && list.batches().size() == 3);
}

LUMEN_TEST(draw_list_surfaces_take_orders_in_paint_order) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    // Two overlapping windows pushed back-to-front by sort key: the front one must sort
    // after the one behind it, whatever the push order.
    list.push(surface_at(2, 0, 1));
    list.push(surface_at(1, 5, 0));
    list.push_quad(quad_at(100, 100), {100, 100, 10, 10}, list.orders().assign({100, 100, 10, 10}));
    list.batch_for_render();
    LUMEN_CHECK(list.size() == 2);
    LUMEN_CHECK(list.data()[0].texture.id() == 1 && list.data()[1].texture.id() == 2);
    const auto refs = list.refs();
    LUMEN_CHECK(refs.size() == 3);
    // The quad does not overlap either window: order 1, before both surfaces.
    LUMEN_CHECK(refs[0].kind == primitive_kind::quad && refs[0].order == 1);
    LUMEN_CHECK(refs[1].kind == primitive_kind::surface && refs[1].index == 0 && refs[1].order == 1);
    LUMEN_CHECK(refs[2].kind == primitive_kind::surface && refs[2].index == 1 && refs[2].order == 2);
    LUMEN_CHECK(list.batches().size() == 2);
    LUMEN_CHECK(list.batches()[1].kind == primitive_kind::surface && list.batches()[1].count == 2);
    // Idempotent: a second pass reuses the orders it assigned.
    list.batch_for_render();
    LUMEN_CHECK(list.refs().size() == 3 && list.refs()[2].order == 2);
}

LUMEN_TEST(draw_list_primitive_between_surfaces_keeps_emission_order) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    // A surface, a quad over it, then a surface over both: each draws over what came
    // before it, whichever way the sort keys or kinds would order them.
    list.push(surface_at(1, 0, 0));
    const lx::rect2i over{0, 0, 10, 10};
    list.push_quad(quad_at(0, 0), over, list.orders().assign(over));
    list.push(surface_at(2, 0, 1));
    list.batch_for_render();
    const auto refs = list.refs();
    LUMEN_CHECK(refs.size() == 3);
    LUMEN_CHECK(refs[0].kind == primitive_kind::surface && refs[0].order == 1 &&
                list.data()[refs[0].index].texture.id() == 1);
    LUMEN_CHECK(refs[1].kind == primitive_kind::quad && refs[1].order == 2);
    LUMEN_CHECK(refs[2].kind == primitive_kind::surface && refs[2].order == 3 &&
                list.data()[refs[2].index].texture.id() == 2);
    LUMEN_CHECK(list.batches().size() == 3);
}

LUMEN_TEST(draw_list_surface_only_list_has_no_stream) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    list.push(surface_at(2, 0, 1));
    list.push(surface_at(1, 5, 0));
    list.batch_for_render();
    // Sorted into paint order, but nothing ordered, nothing batched, the grid untouched.
    LUMEN_CHECK(list.size() == 2 && list.data()[0].texture.id() == 1);
    LUMEN_CHECK(list.refs().empty() && list.batches().empty());
    const lx::scene::draw_list& view = list;
    LUMEN_CHECK(view.orders().max_order() == 0);
}

LUMEN_TEST(draw_list_keeps_batches_cut_before_the_array_grows) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    // Alternating kinds stacked on one rect: every primitive its own order, so every one
    // is its own batch — well past the batch array's first size.
    constexpr unsigned k_runs = lx::scene::k_initial_batches * 4 + 3;
    const lx::rect2i r{0, 0, 10, 10};
    lx::gfx::shadow_instance shadow{};
    for (unsigned i = 0; i < k_runs; ++i) {
        if (i % 2 == 0)
            list.push_quad(quad_at(0, 0), r, list.orders().assign(r));
        else
            list.push_shadow(shadow, r, list.orders().assign(r));
    }
    list.batch_for_render();
    const auto batches = list.batches();
    LUMEN_CHECK(batches.size() == k_runs);
    bool intact = true;
    for (unsigned i = 0; i < batches.size(); ++i) {
        const primitive_kind want = i % 2 == 0 ? primitive_kind::quad : primitive_kind::shadow;
        intact = intact && batches[i].count == 1 && batches[i].first == i &&
                 batches[i].kind == want && batches[i].bounds.width == 10;
    }
    LUMEN_CHECK(intact);
    LUMEN_CHECK(list.overflow_drops() == 0);
}

LUMEN_TEST(draw_list_overflow_counts_drops_and_never_writes_past_the_cap) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    constexpr unsigned k_extra = 5;
    for (unsigned i = 0; i < lx::gfx::k_max_primitives + k_extra; ++i)
        list.push_quad(quad_at(static_cast<int>(i % 64) * 10, static_cast<int>(i / 64) * 10),
                       {static_cast<int>(i % 64) * 10, static_cast<int>(i / 64) * 10, 10, 10}, 1);
    LUMEN_CHECK(list.quad_instances().size() == lx::gfx::k_max_primitives);
    LUMEN_CHECK(list.primitive_count() == lx::gfx::k_max_primitives);
    LUMEN_CHECK(list.overflow_drops() == k_extra);
    // A full primitive stream does not push a surface out.
    list.push(surface_at(1, 0, 0));
    LUMEN_CHECK(list.size() == 1);
    list.batch_for_render();
    LUMEN_CHECK(list.refs().size() == lx::gfx::k_max_primitives + 1);
    LUMEN_CHECK(list.batches().size() == 2);
    LUMEN_CHECK(list.batches()[0].count == lx::gfx::k_max_primitives);
    // `reject` drops too, uncounted as a warn.
    lx::gfx::underline_instance u{};
    list.push_underline(u, {0, 0, 10, 10}, 1, lx::runtime::overflow_action::reject);
    LUMEN_CHECK(list.underline_instances().empty());
}

LUMEN_TEST(draw_list_clear_resets_streams_and_grid) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    const auto order = list.orders().assign({0, 0, 10, 10});
    LUMEN_CHECK(order == 1);
    list.push_quad(quad_at(0, 0), {0, 0, 10, 10}, order);
    list.push(surface_at(1, 0, 0));
    list.batch_for_render();
    LUMEN_CHECK(!list.refs().empty() && !list.batches().empty());

    list.clear();
    LUMEN_CHECK(list.size() == 0);
    LUMEN_CHECK(list.primitive_count() == 0);
    LUMEN_CHECK(list.quad_instances().empty());
    LUMEN_CHECK(list.refs().empty());
    LUMEN_CHECK(list.batches().empty());
    LUMEN_CHECK(list.overflow_drops() == 0);
    LUMEN_CHECK(list.orders().max_order() == 0);
    LUMEN_CHECK(list.orders().view().width == 640); // `clear` keeps the view
    LUMEN_CHECK(list.orders().assign({0, 0, 10, 10}) == 1);
    list.begin({});
    LUMEN_CHECK(list.orders().view().width == 8192);
    list.batch_for_render();
    LUMEN_CHECK(list.refs().empty() && list.batches().empty());
}

LUMEN_TEST(snapshot_exposes_the_sorted_stream) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::snapshot_buffer buf{};
    auto lease = buf.begin_frame();
    LUMEN_CHECK(lease.valid());
    lease.draws().begin({640, 480});
    lease.draws().push_quad(quad_at(0, 0), {0, 0, 10, 10}, 1);
    lease.draws().push(surface_at(1, 20, 0));
    lease.draws().batch_for_render();
    LUMEN_CHECK(buf.publish(lease, {}, 1));
    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = buf.acquire();
    LUMEN_CHECK(snap.refs().size() == 2);
    LUMEN_CHECK(snap.batches().size() == 2);
    LUMEN_CHECK(snap.draws().quad_instances().size() == 1);
    LUMEN_CHECK(snap.draws().size() == 1);
    buf.release(snap);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
}

// ── lx.ui::draw_scope ───────────────────────────────────────────────────────────

LUMEN_TEST(draw_scope_fill_emits_a_solid_draw) {
    lx::scene::draw_list list{};
    list.begin({100, 100});
    lx::ui::draw_scope out{list, {0, 0, 100, 100}};
    out.fill({10, 10, 20, 20}, lx::color::rgb(1.f, 0.5f, 0.f, 0.5f));
    LUMEN_CHECK(out.emitted() == 1);
    LUMEN_CHECK(list.size() == 1);
    const lx::scene::draw_command& cmd = list.data()[0];
    LUMEN_CHECK(cmd.solid);
    LUMEN_CHECK(!cmd.texture);
    LUMEN_CHECK(cmd.dst.x == 10 && cmd.dst.width == 20);
    LUMEN_CHECK(cmd.clip.width == 100);
    LUMEN_CHECK(cmd.opacity == 1.f && cmd.tint.a == 1.f);
    // Premultiplied: rgb × alpha, alpha kept.
    LUMEN_CHECK(cmd.solid_color.r == 0.5f && cmd.solid_color.g == 0.25f &&
                cmd.solid_color.b == 0.f && cmd.solid_color.a == 0.5f);
    // Fully clipped: nothing emitted.
    out.fill({200, 200, 5, 5}, lx::color::rgb(1.f, 1.f, 1.f));
    LUMEN_CHECK(out.emitted() == 1 && list.size() == 1);
}

LUMEN_TEST(draw_scope_drops_fully_clipped_primitives) {
    lx::scene::draw_list list{};
    list.begin({100, 100});
    lx::ui::draw_scope out{list, {0, 0, 100, 100}};
    lx::ui::quad_style style{};
    style.background = lx::color::rgb(1.f, 1.f, 1.f);
    out.quad({100, 0, 10, 10}, style);
    out.quad({0, 100, 10, 10}, style);
    out.quad({0, 0, 0, 10}, style);
    out.underline({-20, 0, 10, 2}, lx::color::rgb(1.f, 1.f, 1.f), 1.f);
    out.sprite(lx::gfx::primitive_kind::mono_sprite, 0, {100, 100, 8, 8}, {}, lx::color::rgb(1.f, 1.f, 1.f));
    out.sprite(lx::gfx::primitive_kind::surface, 0, {0, 0, 8, 8}, {}, lx::color::rgb(1.f, 1.f, 1.f));
    LUMEN_CHECK(out.emitted() == 0);
    LUMEN_CHECK(list.primitive_count() == 0);
    // Partly visible: kept, with its full placed bounds and the clip as the mask.
    out.quad({95, 95, 10, 10}, style);
    LUMEN_CHECK(out.emitted() == 1);
    LUMEN_CHECK(list.quad_instances().size() == 1);
    LUMEN_CHECK(list.quad_instances()[0].bounds[0] == 95.f && list.quad_instances()[0].bounds[2] == 10.f);
    LUMEN_CHECK(list.quad_instances()[0].clip[2] == 100.f);
}

LUMEN_TEST(draw_scope_opacity_multiplies_colours) {
    lx::scene::draw_list list{};
    list.begin({100, 100});
    lx::ui::draw_scope out{list, {0, 0, 100, 100}};
    out.push_opacity(0.5f);
    out.push_opacity(0.5f);
    LUMEN_CHECK(out.opacity() == 0.25f);

    lx::ui::quad_style style{};
    style.background = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    style.border_color = lx::color::rgb(0.f, 0.f, 1.f, 0.8f);
    style.corner_radii[0] = 4.f;
    style.border_widths[2] = 1.f;
    style.border = lx::ui::border_style::dashed;
    out.quad({0, 0, 10, 10}, style);
    const lx::gfx::quad_instance& q = list.quad_instances()[0];
    LUMEN_CHECK(q.background[0] == lx::gfx::to_half(0.25f));
    LUMEN_CHECK(q.background[3] == lx::gfx::to_half(0.25f));
    LUMEN_CHECK(q.background_end[0] == q.background[0]); // solid: bg1 = bg0
    LUMEN_CHECK(q.border_color[2] == lx::gfx::to_half(0.2f) && q.border_color[3] == lx::gfx::to_half(0.2f));
    LUMEN_CHECK(q.radii[0] == lx::gfx::to_half(4.f) && q.radii[1] == 0);
    LUMEN_CHECK(q.border_widths[2] == lx::gfx::to_half(1.f));
    LUMEN_CHECK(q.border_style == 1);

    lx::ui::quad_style gradient = style;
    gradient.background_end = lx::color::rgb(0.f, 0.f, 0.f, 1.f);
    gradient.gradient_angle_deg = 90.f;
    out.quad({20, 0, 10, 10}, gradient);
    const lx::gfx::quad_instance& g = list.quad_instances()[1];
    LUMEN_CHECK(g.background_end[0] == 0 && g.background_end[3] == lx::gfx::to_half(0.25f));
    LUMEN_CHECK(g.gradient_angle == lx::gfx::to_half(90.f));

    out.underline({0, 20, 10, 2}, lx::color::rgb(1.f, 0.f, 0.f, 1.f), 1.5f, true);
    const lx::gfx::underline_instance& u = list.underline_instances()[0];
    LUMEN_CHECK(u.color[0] == lx::gfx::to_half(0.25f) && u.color[3] == lx::gfx::to_half(0.25f));
    LUMEN_CHECK(u.thickness == lx::gfx::to_half(1.5f) && u.wavy == 1);

    out.fill({0, 40, 10, 10}, lx::color::rgb(1.f, 1.f, 1.f, 1.f));
    LUMEN_CHECK(list.data()[0].solid_color.a == 0.25f && list.data()[0].solid_color.r == 0.25f);

    out.pop();
    out.pop();
    out.fill({0, 60, 10, 10}, lx::color::rgb(1.f, 1.f, 1.f, 1.f));
    LUMEN_CHECK(list.data()[1].solid_color.a == 1.f);
}

LUMEN_TEST(draw_scope_nested_origins_offset_and_clips_intersect) {
    lx::scene::draw_list list{};
    list.begin({100, 100});
    lx::ui::draw_scope out{list, {0, 0, 100, 100}};
    out.push_origin({10, 10});
    out.push_origin({5, 5});
    lx::ui::quad_style style{};
    style.background = lx::color::rgb(1.f, 1.f, 1.f);
    out.quad({0, 0, 10, 10}, style);
    LUMEN_CHECK(list.quad_instances()[0].bounds[0] == 15.f && list.quad_instances()[0].bounds[1] == 15.f);
    // A clip pushed inside the offset frame is in absolute coordinates, like `clip()`.
    out.push_clip({0, 0, 20, 20});
    LUMEN_CHECK(out.clip().width == 20);
    out.quad({0, 0, 30, 30}, style); // placed at 15,15: 5 × 5 px visible
    const lx::gfx::quad_instance& q = list.quad_instances()[1];
    LUMEN_CHECK(q.bounds[0] == 15.f && q.bounds[2] == 30.f);
    LUMEN_CHECK(q.clip[0] == 0.f && q.clip[2] == 20.f);
    out.quad({10, 10, 10, 10}, style); // placed at 25,25: hidden by the clip
    LUMEN_CHECK(out.emitted() == 2);
    out.pop();
    out.quad({10, 10, 10, 10}, style);
    LUMEN_CHECK(out.emitted() == 3);
    out.pop();
    out.pop();
    out.quad({10, 10, 10, 10}, style);
    LUMEN_CHECK(list.quad_instances()[3].bounds[0] == 10.f);

    list.batch_for_render();
    // Batch bounds are the visible parts: the second quad contributes its clipped 5 × 5.
    LUMEN_CHECK(list.batches().size() >= 1);
    lx::rect2i all{};
    for (const lx::gfx::batch& b : list.batches()) {
        if (all.width == 0) {
            all = b.bounds;
            continue;
        }
        const int x1 = std::max(all.x + all.width, b.bounds.x + b.bounds.width);
        const int y1 = std::max(all.y + all.height, b.bounds.y + b.bounds.height);
        all.x = std::min(all.x, b.bounds.x);
        all.y = std::min(all.y, b.bounds.y);
        all.width = x1 - all.x;
        all.height = y1 - all.y;
    }
    LUMEN_CHECK(all.x == 10 && all.y == 10);
    LUMEN_CHECK(all.x + all.width == 35 && all.y + all.height == 35);
}

LUMEN_TEST(draw_scope_shadow_extent_and_sprite_params) {
    lx::scene::draw_list list{};
    list.begin({100, 100});
    lx::ui::draw_scope out{list, {0, 0, 100, 100}};
    lx::ui::shadow_style shadow{};
    shadow.color = lx::color::rgb(0.f, 0.f, 0.f, 0.5f);
    shadow.blur_sigma = 4.f;
    shadow.spread = 1.f;
    // The box is off the clip, its blur (3σ + spread = 13 px) is not.
    out.shadow({105, 10, 20, 20}, shadow);
    LUMEN_CHECK(out.emitted() == 1);
    const lx::gfx::shadow_instance& s = list.shadow_instances()[0];
    LUMEN_CHECK(s.bounds[0] == 105.f && s.sigma == 4.f && s.spread == 1.f && s.flags == 0);
    LUMEN_CHECK(s.color[3] == lx::gfx::to_half(0.5f));
    out.shadow({120, 10, 20, 20}, shadow); // 13 px short of the clip
    LUMEN_CHECK(out.emitted() == 1);
    shadow.inset = true;
    out.shadow({105, 10, 20, 20}, shadow); // inset: only the box counts
    LUMEN_CHECK(out.emitted() == 1);
    out.shadow({90, 10, 20, 20}, shadow);
    LUMEN_CHECK(out.emitted() == 2 && list.shadow_instances()[1].flags == 1);

    lx::gfx::sprite_uv uv{3, 5, 11, 13};
    out.sprite(lx::gfx::primitive_kind::mono_sprite, 2, {0, 0, 8, 8}, uv, lx::color::rgb(1.f, 1.f, 1.f));
    out.sprite(lx::gfx::primitive_kind::poly_sprite, 1, {10, 0, 8, 8}, uv, lx::color::rgb(1.f, 1.f, 1.f),
               {2.f, 2.f, 0.f, 0.f});
    out.sprite(lx::gfx::primitive_kind::path, 0, {20, 0, 8, 8}, uv, lx::color::rgb(1.f, 0.f, 0.f));
    LUMEN_CHECK(list.sprite_instances().size() == 3);
    const lx::gfx::sprite_instance& mono = list.sprite_instances()[0];
    LUMEN_CHECK(mono.uv[0] == 3 && mono.uv[1] == 5 && mono.uv[2] == 11 && mono.uv[3] == 13);
    LUMEN_CHECK(mono.page == 2);
    // No rotation given: the identity, not a collapsing zero matrix.
    LUMEN_CHECK(mono.params[0] == lx::gfx::to_half(1.f) && mono.params[1] == 0 &&
                mono.params[3] == lx::gfx::to_half(1.f));
    const lx::gfx::sprite_instance& poly = list.sprite_instances()[1];
    LUMEN_CHECK(poly.params[0] == lx::gfx::to_half(2.f) && poly.params[2] == 0);
    list.batch_for_render();
    // Three sprite kinds and pages: three batches, the ref carries the page.
    LUMEN_CHECK(list.batches().size() == 5);
    bool saw_page_2 = false;
    for (const lx::gfx::primitive_ref& r : list.refs())
        saw_page_2 = saw_page_2 || (r.kind == lx::gfx::primitive_kind::mono_sprite && r.page == 2);
    LUMEN_CHECK(saw_page_2);
}

LUMEN_TEST(draw_scope_orders_overlapping_emits_above_each_other) {
    lx::scene::draw_list list{};
    list.begin({100, 100});
    lx::ui::draw_scope out{list, {0, 0, 100, 100}};
    lx::ui::quad_style style{};
    style.background = lx::color::rgb(1.f, 1.f, 1.f);
    out.quad({0, 0, 50, 50}, style);
    out.fill({10, 10, 10, 10}, lx::color::rgb(1.f, 0.f, 0.f)); // over the quad
    out.quad({70, 70, 10, 10}, style);                          // beside both
    list.batch_for_render();
    const auto refs = list.refs();
    LUMEN_CHECK(refs.size() == 3);
    LUMEN_CHECK(refs[0].kind == primitive_kind::quad && refs[0].order == 1);
    LUMEN_CHECK(refs[1].kind == primitive_kind::quad && refs[1].order == 1);
    LUMEN_CHECK(refs[2].kind == primitive_kind::surface && refs[2].order == 2);
    LUMEN_CHECK(list.batches().size() == 2);
}

// ── lx.ui::ui_root idle rule ────────────────────────────────────────────────────

namespace {

lx::ui::child describe_panel(lx::ui::build_context&) {
    return lx::ui::box(lx::color::rgb(0.1f, 0.1f, 0.1f),
                       lx::ui::button("ok", nullptr, lx::color::rgb(0.2f, 0.3f, 0.5f)));
}

lx::ui::child describe_panel_without_button(lx::ui::build_context&) {
    return lx::ui::box(lx::color::rgb(0.1f, 0.1f, 0.1f), lx::ui::text("ok"));
}

/// Asks for `frames` more frames from its own emit, as an animation does.
struct animated_props {
    unsigned frames = 0;
};

class animated_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    void apply(const animated_props& props) { left_ = props.frames; }
    [[nodiscard]] lx::size2i measure(lx::layout::constraints, lx::ui::size_hint) const override {
        return {20, 20};
    }
    void emit(lx::ui::draw_scope& out) const override {
        out.fill(bounds(), lx::color::rgb(1.f, 1.f, 1.f));
        if (left_ > 0) {
            --left_;
            lx::ui::ui_damage().request(lx::ui::invalidation::animate);
        }
    }

private:
    mutable unsigned left_ = 0;
};

lx::ui::child describe_animated(lx::ui::build_context&) {
    return lx::ui::describe<animated_node, animated_props>({2});
}

} // namespace

LUMEN_TEST(ui_root_tick_skips_an_idle_frame) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::ui::ui_damage().clear();
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::ui_root root{arena};
    root.set_describer(describe_panel);
    root.set_bounds({0, 0, 200, 100});

    lx::scene::draw_list draws{};
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(root.tree() != nullptr);
    const unsigned first = draws.size();
    LUMEN_CHECK(first > 0);
    LUMEN_CHECK(lx::ui::ui_damage().empty());

    // Nothing changed: no frame, and the list is not even cleared.
    draws.push(surface_at(9, 0, 0));
    LUMEN_CHECK(!root.tick(draws));
    LUMEN_CHECK(draws.size() == first + 1);
    LUMEN_CHECK(!root.tick(draws));

    // A paint invalidation produces a frame again, rebuilt from scratch.
    lx::ui::invalidate_paint(*root.tree());
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(draws.size() == first);
    LUMEN_CHECK(!root.tick(draws));

    // An animation request is exactly one frame.
    lx::ui::invalidate_animate(*root.tree());
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(!root.tick(draws));

    // A layout invalidation re-describes; new bounds too.
    lx::ui::invalidate_layout(*root.tree());
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(!root.tick(draws));
    root.set_bounds({0, 0, 300, 100});
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(!root.tick(draws));
    // The solid fills reached the list as solid draws.
    LUMEN_CHECK(draws.size() == first && draws.data()[0].solid);
}

LUMEN_TEST(ui_root_keeps_what_the_emit_asks_for) {
    lx::ui::ui_damage().clear();
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::ui_root root{arena};
    root.set_describer(describe_animated);
    root.set_bounds({0, 0, 100, 100});
    lx::scene::draw_list draws{};
    // The node asks for two more frames from its emit: the ledger must still hold each
    // request when the tick returns, or the animation stops after its first frame.
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(lx::ui::ui_damage().needs_animate());
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(lx::ui::ui_damage().needs_animate());
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(lx::ui::ui_damage().empty());
    LUMEN_CHECK(!root.tick(draws));
}

LUMEN_TEST(ui_root_frame_damage_holds_a_removed_node) {
    lx::ui::ui_damage().clear();
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::ui_root root{arena};
    root.set_describer(describe_panel);
    root.set_bounds({0, 0, 200, 100});
    lx::scene::draw_list draws{};
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(!root.tick(draws));

    // The button gives way to a text node: the reconcile damages the bounds it vacates.
    // That damage is this frame's (it repaints the area), not a reason for another frame.
    root.set_describer(describe_panel_without_button);
    LUMEN_CHECK(root.tick(draws));
    LUMEN_CHECK(root.frame_damage().count() > 0);
    LUMEN_CHECK(lx::ui::ui_damage().empty());
    LUMEN_CHECK(!root.tick(draws));
}

int main(int argc, char** argv) {
    // `draw_scope` and `ui_root::tick` are UI affinity.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
