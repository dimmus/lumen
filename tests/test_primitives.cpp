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

/// A deterministic sequence for the randomised cases (xorshift64).
struct test_rng {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    [[nodiscard]] std::uint64_t next() {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return state;
    }
    [[nodiscard]] unsigned below(unsigned n) { return static_cast<unsigned>(next() % n); }
};

[[nodiscard]] bool same_ref(const lx::gfx::primitive_ref& a, const lx::gfx::primitive_ref& b) {
    return a.order == b.order && a.kind == b.kind && a.page == b.page && a.index == b.index;
}

[[nodiscard]] bool same_quad(const lx::gfx::quad_instance& a, const lx::gfx::quad_instance& b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
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

LUMEN_TEST(half_fast_path_matches_the_reference_bit_for_bit) {
    // `to_half` takes integer shortcuts for normal and overflowing magnitudes and for
    // zero; `to_half_reference` is the case-by-case conversion. Every sign and exponent,
    // with every half mantissa against each rounding edge of the 13 dropped bits (just
    // below, at and just past the tie, the tie's neighbours) plus pseudo-random low bits:
    // 2 × 256 × 8192 floats, well under a second. (An exhaustive 2^32 sweep agreed when the
    // fast path was written; it takes ~15 s, too long for every test run.)
    static_assert(lx::gfx::to_half(0.5f) == lx::gfx::to_half_reference(0.5f));
    static_assert(lx::gfx::to_half(65519.f) == lx::gfx::to_half_reference(65519.f));
    constexpr std::uint32_t k_edges[8] = {0x0000u, 0x0001u, 0x0FFFu, 0x1000u,
                                          0x1001u, 0x1FFFu, 0x0800u, 0x17FFu};
    test_rng rng{};
    unsigned mismatches = 0;
    std::uint32_t first_bad = 0;
    const auto check = [&](std::uint32_t bits) {
        const float f = std::bit_cast<float>(bits);
        if (lx::gfx::to_half(f) != lx::gfx::to_half_reference(f)) {
            if (mismatches++ == 0)
                first_bad = bits;
        }
    };
    for (std::uint32_t sign = 0; sign < 2; ++sign) {
        for (std::uint32_t exp = 0; exp < 256; ++exp) {
            const std::uint32_t high = (sign << 31u) | (exp << 23u);
            for (std::uint32_t m10 = 0; m10 < 1024; ++m10) {
                for (const std::uint32_t low : k_edges)
                    check(high | (m10 << 13u) | low);
                check(high | (m10 << 13u) | static_cast<std::uint32_t>(rng.next() & 0x1FFFu));
            }
            for (unsigned i = 0; i < 1024; ++i)
                check(high | static_cast<std::uint32_t>(rng.next() & 0x7FFFFFu));
        }
    }
    // The fast path's own boundaries: the smallest normal half and its predecessor, half
    // the smallest subnormal, the clamp at 65504 and the first float that would round to
    // infinity, the largest finite float, infinity and NaN payloads.
    for (const std::uint32_t bits :
         {0x38800000u, 0x387FFFFFu, 0x33000000u, 0x32FFFFFFu, 0x33000001u, 0x477FE000u,
          0x477FDFFFu, 0x477FEFFFu, 0x477FF000u, 0x47800000u, 0x7F7FFFFFu, 0x7F800000u,
          0x7F800001u, 0x7FC00000u, 0xFFFFFFFFu, 0x80000000u, 0x00000001u})
        check(bits);
    if (mismatches != 0)
        std::printf("to_half mismatch: %u floats, first 0x%08x\n", mismatches, first_bad);
    LUMEN_CHECK(mismatches == 0);
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

LUMEN_TEST(order_grid_one_cell_fast_path_matches_the_cell_loop) {
    // A model of the grid's rule over the same cells: `1 + max` under the rect (at least
    // the layer base), written back. Rects inside one cell take the fast path, the rest the
    // loops; both must agree with the model through layers and off-view rects.
    lx::gfx::order_grid g{};
    g.reset({640, 480}); // 10 × 8 cells of 64 px
    std::uint32_t model[80]{};
    std::uint32_t base = 0;
    std::uint32_t bases[8]{};
    unsigned depth = 0;
    std::uint32_t max_order = 0;
    test_rng rng{};
    bool agree = true;
    unsigned one_cell = 0;
    for (unsigned i = 0; i < 4000; ++i) {
        if (i % 500 == 250 && depth < 8) {
            g.push_layer();
            bases[depth++] = base;
            base = max_order + 1;
        } else if (i % 500 == 499 && depth > 0) {
            g.pop_layer();
            base = bases[--depth];
        }
        // Mostly small rects (one cell or straddling two), some large, some off-view.
        const bool big = rng.below(10) == 0;
        const int w = big ? 1 + static_cast<int>(rng.below(400)) : 1 + static_cast<int>(rng.below(20));
        const int h = big ? 1 + static_cast<int>(rng.below(300)) : 1 + static_cast<int>(rng.below(20));
        const int x = static_cast<int>(rng.below(700)) - 30;
        const int y = static_cast<int>(rng.below(540)) - 30;
        const std::uint32_t got = g.assign({x, y, w, h});
        const int x0 = x < 0 ? 0 : x;
        const int y0 = y < 0 ? 0 : y;
        const int x1 = x + w > 640 ? 640 : x + w;
        const int y1 = y + h > 480 ? 480 : y + h;
        std::uint32_t want = base;
        if (x1 > x0 && y1 > y0) {
            std::uint32_t highest = 0;
            for (int cy = y0 / 64; cy <= (y1 - 1) / 64; ++cy)
                for (int cx = x0 / 64; cx <= (x1 - 1) / 64; ++cx)
                    highest = model[cy * 10 + cx] > highest ? model[cy * 10 + cx] : highest;
            want = highest + 1 > base ? highest + 1 : base;
            for (int cy = y0 / 64; cy <= (y1 - 1) / 64; ++cy)
                for (int cx = x0 / 64; cx <= (x1 - 1) / 64; ++cx)
                    model[cy * 10 + cx] = want;
            max_order = want > max_order ? want : max_order;
            one_cell += (x0 / 64 == (x1 - 1) / 64 && y0 / 64 == (y1 - 1) / 64) ? 1u : 0u;
        }
        agree = agree && got == want;
    }
    LUMEN_CHECK(agree);
    LUMEN_CHECK(g.max_order() == max_order);
    LUMEN_CHECK(one_cell > 1000); // the fast path was exercised
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

LUMEN_TEST(draw_list_ascending_stream_keeps_push_order) {
    // Keys already ascending (each primitive above the last): the sort is skipped and the
    // stream is the push order.
    lx::scene::draw_list list{};
    list.begin({640, 480});
    const lx::rect2i r{0, 0, 10, 10};
    lx::gfx::shadow_instance shadow{};
    for (unsigned i = 0; i < 300; ++i) {
        if (i % 3 == 0)
            list.push_shadow(shadow, r, list.orders().assign(r));
        else
            list.push_quad(quad_at(0, 0), r, list.orders().assign(r));
    }
    list.batch_for_render();
    const auto refs = list.refs();
    LUMEN_CHECK(refs.size() == 300);
    bool in_push_order = true;
    for (unsigned i = 0; i < refs.size(); ++i)
        in_push_order = in_push_order && refs[i].order == i + 1 &&
                        refs[i].kind == (i % 3 == 0 ? primitive_kind::shadow : primitive_kind::quad);
    LUMEN_CHECK(in_push_order);
    LUMEN_CHECK(list.batches().size() == 200); // shadow, quad quad, shadow, …
}

LUMEN_TEST(draw_list_one_kind_stream_is_one_batch_in_push_order) {
    // Only quads, orders not ascending: one batch whatever the orders, so the stream stays
    // in push order (overlapping quads keep their paint order) and nothing is sorted.
    lx::scene::draw_list list{};
    list.begin({640, 480});
    list.push_quad(quad_at(0, 0), {0, 0, 50, 50}, 1);
    list.push_quad(quad_at(10, 10), {10, 10, 10, 10}, 2); // over the first
    list.push_quad(quad_at(300, 200), {300, 200, 20, 30}, 1);
    list.push_quad(quad_at(0, 0), {0, 0, 0, 0}, 1); // empty bounds: no part of the union
    list.batch_for_render();
    const auto refs = list.refs();
    LUMEN_CHECK(refs.size() == 4);
    bool in_push_order = true;
    for (unsigned i = 0; i < refs.size(); ++i)
        in_push_order = in_push_order && refs[i].index == i && refs[i].kind == primitive_kind::quad;
    LUMEN_CHECK(in_push_order);
    const auto batches = list.batches();
    LUMEN_CHECK(batches.size() == 1);
    LUMEN_CHECK(batches[0].kind == primitive_kind::quad && batches[0].first == 0 &&
                batches[0].count == 4);
    LUMEN_CHECK(batches[0].bounds.x == 0 && batches[0].bounds.y == 0 &&
                batches[0].bounds.width == 320 && batches[0].bounds.height == 230);

    // A shadow joins: two kinds, the full sort again (the order-1 quads before the order-2).
    lx::gfx::shadow_instance shadow{};
    list.push_shadow(shadow, {500, 0, 10, 10}, 3);
    list.batch_for_render();
    LUMEN_CHECK(list.refs().size() == 5 && list.refs()[2].order == 1 &&
                list.refs()[3].order == 2 && list.batches().size() == 2);

    // Only shadows: the same rule for another kind.
    list.begin({640, 480});
    list.push_shadow(shadow, {0, 0, 10, 10}, 2);
    list.push_shadow(shadow, {20, 0, 10, 10}, 1);
    list.batch_for_render();
    LUMEN_CHECK(list.refs().size() == 2 && list.refs()[0].order == 2 &&
                list.batches().size() == 1 && list.batches()[0].kind == primitive_kind::shadow &&
                list.batches()[0].bounds.width == 30);
}

LUMEN_TEST(draw_list_radix_sort_matches_a_stable_comparison_sort) {
    // Orders spanning three bytes, every kind, several pages, pushed in random order: the
    // radix sort must give exactly what a stable comparison sort of the push-order refs by
    // (order, kind, page) gives.
    lx::scene::draw_list list{};
    list.begin({640, 480});
    test_rng rng{};
    lx::gfx::shadow_instance shadow{};
    lx::gfx::sprite_instance sprite{};
    lx::gfx::underline_instance underline{};
    std::vector<lx::gfx::primitive_ref> pushed{};
    unsigned quads = 0, shadows = 0, sprites = 0, underlines = 0;
    constexpr unsigned k_count = 3000;
    for (unsigned i = 0; i < k_count; ++i) {
        // Few distinct small orders, so equal keys (where stability shows) are common, and
        // a quarter past 2^16 so the order's third byte varies too.
        const std::uint32_t order = rng.below(4) == 0 ? 65536u + rng.below(70000) : rng.below(40);
        const lx::rect2i r{static_cast<int>(rng.below(600)), static_cast<int>(rng.below(440)), 8, 8};
        switch (rng.below(5)) {
        case 0:
            list.push_quad(quad_at(r.x, r.y), r, order);
            pushed.push_back({order, primitive_kind::quad, 0, 0, quads++});
            break;
        case 1:
            list.push_shadow(shadow, r, order);
            pushed.push_back({order, primitive_kind::shadow, 0, 0, shadows++});
            break;
        case 2: {
            const auto page = static_cast<std::uint8_t>(rng.below(3));
            list.push_sprite(primitive_kind::mono_sprite, page, sprite, r, order);
            pushed.push_back({order, primitive_kind::mono_sprite, page, 0, sprites++});
            break;
        }
        case 3: {
            const auto page = static_cast<std::uint8_t>(rng.below(2));
            list.push_sprite(primitive_kind::poly_sprite, page, sprite, r, order);
            pushed.push_back({order, primitive_kind::poly_sprite, page, 0, sprites++});
            break;
        }
        default:
            list.push_underline(underline, r, order);
            pushed.push_back({order, primitive_kind::underline, 0, 0, underlines++});
            break;
        }
    }
    // Surfaces in paint order (ascending sort key), so their refs follow in push order.
    for (unsigned i = 0; i < 20; ++i) {
        const std::uint32_t order = rng.below(40);
        list.push(surface_at(i + 1, static_cast<int>(i) * 20, i), order);
        pushed.push_back({order, primitive_kind::surface, 0, 0, i});
    }
    std::stable_sort(pushed.begin(), pushed.end(),
                     [](const lx::gfx::primitive_ref& a, const lx::gfx::primitive_ref& b) {
                         if (a.order != b.order)
                             return a.order < b.order;
                         if (a.kind != b.kind)
                             return static_cast<int>(a.kind) < static_cast<int>(b.kind);
                         return a.page < b.page;
                     });

    for (unsigned pass = 0; pass < 2; ++pass) {
        // The second pass sorts again from the untouched push order: the same stream.
        list.batch_for_render();
        const auto refs = list.refs();
        bool same = refs.size() == pushed.size();
        for (unsigned i = 0; same && i < refs.size(); ++i)
            same = same_ref(refs[i], pushed[i]);
        LUMEN_CHECK(same);
        // Batches partition the stream at every (kind, page) change.
        unsigned covered = 0;
        bool cut_right = true;
        for (const lx::gfx::batch& b : list.batches()) {
            cut_right = cut_right && b.first == covered && b.count > 0;
            for (unsigned i = b.first; i < b.first + b.count; ++i)
                cut_right = cut_right && refs[i].kind == b.kind && refs[i].page == b.page;
            covered += b.count;
        }
        LUMEN_CHECK(cut_right && covered == refs.size());
    }
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

LUMEN_TEST(encode_instances_copies_runs_and_gathers_permuted_batches) {
    lx::scene::draw_list list{};
    list.begin({640, 480});
    // Batch 0: quads 0..3 in push order (one block copy). Batch 1: a shadow. Batch 2: quads
    // 4 and 5, which the sort swaps (5 has the lower order): a gather. Batch 3: a surface.
    for (int i = 0; i < 4; ++i)
        list.push_quad(quad_at(i * 10, 0), {i * 10, 0, 10, 10}, 1);
    lx::gfx::shadow_instance shadow{};
    shadow.sigma = 7.f;
    list.push_shadow(shadow, {0, 20, 10, 10}, 2);
    list.push_quad(quad_at(100, 0), {100, 0, 10, 10}, 4);
    list.push_quad(quad_at(110, 0), {110, 0, 10, 10}, 3);
    list.push(surface_at(1, 200, 0), 5u);
    list.batch_for_render();
    const auto batches = list.batches();
    LUMEN_CHECK(batches.size() == 4);
    LUMEN_CHECK(batches[2].kind == primitive_kind::quad && batches[2].count == 2);

    const auto quads = list.quad_instances();
    std::vector<std::byte> ring(4096);
    std::uint32_t offsets[4] = {~0u, ~0u, ~0u, ~0u};
    const std::size_t bytes = list.encode_instances(ring, offsets);
    constexpr std::size_t q = sizeof(lx::gfx::quad_instance);
    constexpr std::size_t sh = sizeof(lx::gfx::shadow_instance);
    LUMEN_CHECK(bytes == 6 * q + sh);
    LUMEN_CHECK(offsets[0] == 0 && offsets[1] == 4 * q && offsets[2] == 4 * q + sh &&
                offsets[3] == bytes); // the surface batch copies nothing
    LUMEN_CHECK(std::memcmp(ring.data(), quads.data(), 4 * q) == 0);
    LUMEN_CHECK(std::memcmp(ring.data() + 4 * q, &shadow, sh) == 0);
    LUMEN_CHECK(std::memcmp(ring.data() + 4 * q + sh, &quads[5], q) == 0);
    LUMEN_CHECK(std::memcmp(ring.data() + 5 * q + sh, &quads[4], q) == 0);

    // Batch starts aligned to 256 bytes: 0, 512 (after 320), 768 (after 576).
    std::uint32_t aligned[4]{};
    const std::size_t padded = list.encode_instances(ring, aligned, 256);
    LUMEN_CHECK(aligned[0] == 0 && aligned[1] == 512 && aligned[2] == 768);
    LUMEN_CHECK(padded == 768 + 2 * q);
    LUMEN_CHECK(std::memcmp(ring.data() + 512, &shadow, sh) == 0);
    LUMEN_CHECK(std::memcmp(ring.data() + 768, &quads[5], q) == 0);

    // A ring too small: the batches that fit whole are written (quads, shadow: 384 of 400
    // bytes), the last is not, and the result says how much was needed.
    std::vector<std::byte> small(5 * q, std::byte{0xAB});
    LUMEN_CHECK(list.encode_instances(small) == bytes);
    LUMEN_CHECK(std::memcmp(small.data(), quads.data(), 4 * q) == 0);
    LUMEN_CHECK(std::memcmp(small.data() + 4 * q, &shadow, sh) == 0);
    LUMEN_CHECK(small[4 * q + sh] == std::byte{0xAB} && small[5 * q - 1] == std::byte{0xAB});
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

LUMEN_TEST(draw_scope_packed_quad_style_matches_the_unpacked_one) {
    lx::scene::draw_list list{};
    list.begin({200, 100});
    lx::ui::draw_scope out{list, {0, 0, 200, 100}};
    lx::ui::quad_style style{};
    style.background = lx::color::rgb(0.3f, 0.6f, 0.9f, 0.7f);
    style.background_end = lx::color::rgb(0.1f, 0.2f, 0.3f, 1.f);
    style.gradient_angle_deg = 45.f;
    style.border_color = lx::color::rgb(1.f, 0.5f, 0.25f, 0.5f);
    style.corner_radii[0] = 2.f;
    style.corner_radii[2] = 6.5f;
    style.border_widths[1] = 1.5f;
    style.border = lx::ui::border_style::dashed;
    const lx::ui::packed_quad_style packed = lx::ui::pack_quad_style(style);

    // Opacity 1: the packed form is copied, bit for bit what the float style writes.
    out.quad({0, 0, 10, 10}, style);
    out.quad({0, 0, 10, 10}, packed);
    LUMEN_CHECK(list.quad_instances().size() == 2);
    lx::gfx::quad_instance a = list.quad_instances()[0];
    lx::gfx::quad_instance b = list.quad_instances()[1];
    LUMEN_CHECK(same_quad(a, b));
    LUMEN_CHECK(a.background[3] == lx::gfx::to_half(0.7f) && a.border_style == 1);

    // Below 1 the packed colours are rescaled from their halves: within one half-float ulp
    // of folding the opacity in before the premultiply. Geometry and shape are unchanged.
    out.push_opacity(0.37f);
    out.quad({20, 0, 10, 10}, style);
    out.quad({20, 0, 10, 10}, packed);
    a = list.quad_instances()[2];
    b = list.quad_instances()[3];
    const auto near = [](std::uint16_t x, std::uint16_t y) {
        return (x > y ? x - y : y - x) <= 1;
    };
    bool close = true;
    for (unsigned i = 0; i < 4; ++i) {
        close = close && near(a.background[i], b.background[i]) &&
                near(a.background_end[i], b.background_end[i]) &&
                near(a.border_color[i], b.border_color[i]);
    }
    LUMEN_CHECK(close);
    LUMEN_CHECK(std::memcmp(a.radii, b.radii, sizeof a.radii) == 0);
    LUMEN_CHECK(std::memcmp(a.border_widths, b.border_widths, sizeof a.border_widths) == 0);
    LUMEN_CHECK(std::memcmp(a.bounds, b.bounds, sizeof a.bounds) == 0);
    LUMEN_CHECK(a.gradient_angle == b.gradient_angle && b.border_style == 1);
    out.pop();

    // A clipped-away packed quad is dropped like any other.
    out.quad({500, 0, 10, 10}, packed);
    LUMEN_CHECK(list.quad_instances().size() == 4);
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
