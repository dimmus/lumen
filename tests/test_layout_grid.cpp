// lx.layout:grid's solver and lx.ui's grid container (ADR-010 Phase 5 "grid containers").
//
// Expected numbers are, case by case, arithmetic from the CSS Grid algorithm as restricted
// by lx.layout:grid (fixed, percent, auto and fr tracks; sparse row-major auto-placement;
// spans; gaps; padding) — written out in the comment beside each case.

#include "lumen_test.hpp"

import std;

import lx.foundation;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.ui;

namespace {

using lx::layout::auto_length;
using lx::layout::grid_children;
using lx::layout::grid_item_state;
using lx::layout::grid_output;
using lx::layout::grid_input;
using lx::layout::grid_placement;
using lx::layout::grid_row_state;
using lx::layout::grid_scratch;
using lx::layout::grid_style;
using lx::layout::grid_track;
using lx::layout::percent;
using lx::layout::px;

struct item {
    float w = 10.f, h = 10.f;
};

struct fixture {
    std::vector<item> items{};
    std::vector<grid_placement> placements{};

    // The children and the scratch, built in this order (the spans must see the resized
    // vectors).
    grid_children children(std::vector<grid_item_state>& state, std::vector<lx::rect2f>& rects,
                           std::vector<grid_row_state>& rows) {
        state.resize(items.size());
        rects.resize(items.size());
        rows.resize(std::max<std::size_t>(items.size(), 1));
        return grid_children{this, static_cast<unsigned>(items.size()), placement_cb,
                             measure_cb};
    }

    static const grid_placement* placement_cb(void* ctx, unsigned i) {
        auto& f = *static_cast<fixture*>(ctx);
        return i < f.placements.size() ? &f.placements[i] : nullptr;
    }
    static lx::layout::flex_size measure_cb(void* ctx, unsigned i,
                                            const lx::layout::child_query&) {
        auto& f = *static_cast<fixture*>(ctx);
        return {f.items[i].w, f.items[i].h};
    }
};

grid_input definite_input(float w, float h) {
    grid_input in{};
    in.known_width = w;
    in.known_height = h;
    in.available_width = w;
    in.available_height = h;
    in.rem_px = 16.f;
    return in;
}

grid_input auto_input(float available_w = std::numeric_limits<float>::infinity()) {
    grid_input in{};
    in.available_width = available_w;
    in.available_height = std::numeric_limits<float>::infinity();
    in.rem_px = 16.f;
    return in;
}

/// Runs one solve with the fixture's storage.
[[nodiscard]] lx::result<grid_output> run(fixture& f, const grid_style& gs,
                                          const grid_input& in,
                                          std::vector<lx::rect2f>& rects) {
    std::vector<grid_item_state> state;
    std::vector<grid_row_state> rows;
    auto ch = f.children(state, rects, rows);
    return lx::layout::solve_grid(gs, in, ch, rects, {state, rects, rows});
}

/// A fixed-size leaf.
struct box_props {
    float w = 10.f, h = 10.f;
};
class box_leaf final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "box"; }
    bool apply(const box_props& p) {
        props_ = p;
        return true;
    }
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c,
                                     lx::ui::size_hint) const override {
        lx::size2i out{static_cast<int>(props_.w), static_cast<int>(props_.h)};
        out.width = std::clamp(out.width, c.min.width, std::max(c.min.width, c.max.width));
        out.height = std::clamp(out.height, c.min.height, std::max(c.min.height, c.max.height));
        return out;
    }

private:
    box_props props_{};
};

[[nodiscard]] lx::ui::child box(float w, float h, lx::ui::element_key key = 0) {
    return lx::ui::describe<box_leaf, box_props>({w, h}, key);
}

[[nodiscard]] bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// Builds `describe` into a retained tree through `ui_root` and lays it out at `bounds`.
struct ui_harness {
    lx::runtime::memory_arena arena{256 * 1024};
    lx::ui::ui_root root{arena};
    lx::scene::draw_list draws{};

    ui_harness(lx::ui::describe_fn describe, lx::rect2i bounds) {
        lx::ui::ui_damage().clear();
        root.set_describer(describe);
        root.set_bounds(bounds);
        (void)root.tick(draws);
    }
    [[nodiscard]] lx::ui::ui_node& node(std::initializer_list<unsigned> path) const {
        lx::ui::ui_node* n = root.tree();
        for (unsigned i : path)
            n = n->child_at(i);
        return *n;
    }
};

struct describe_two_cells_t {
    static lx::ui::child build(lx::ui::build_context&) {
        lx::layout::grid_style gs{};
        gs.columns = 2;
        gs.column[0] = {px(100), 0.f};
        gs.column[1] = {px(100), 0.f};
        lx::ui::child cells[2] = {
            lx::ui::placed({.column = 1, .row = 1}, box(200, 40)),
            lx::ui::placed({.column = 2, .row = 2}, box(200, 40)),
        };
        return lx::ui::grid(gs, cells);
    }
};

} // namespace

LUMEN_TEST(grid_fixed_tracks_and_auto_placement) {
    // Four 10×10 items in 100 px + 60 px columns, 6 px column and 4 px row gaps: rows of
    // two, row 1 at y = 10 + 4; the definite size wins, the content height stops at the
    // last row (the gap after it is not content).
    fixture f;
    f.items = {{}, {}, {}, {}};
    grid_style gs{};
    gs.columns = 2;
    gs.column[0] = {px(100), 0.f};
    gs.column[1] = {px(60), 0.f};
    gs.row_gap = px(4);
    gs.column_gap = px(6);
    std::vector<lx::rect2f> rects;
    auto r = run(f, gs, definite_input(166, 100), rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[0].x == 0 && rects[0].y == 0 && rects[0].width == 100);
    LUMEN_CHECK(rects[1].x == 106 && rects[1].width == 60);
    LUMEN_CHECK(rects[2].y == 14 && rects[3].x == 106);
    LUMEN_CHECK(r->width == 166 && r->height == 100);
    LUMEN_CHECK(r->content_height == 24); // two rows and the gap between them
}

LUMEN_TEST(grid_percent_and_fr_columns) {
    // 200 px: a 50 % column and one fr — the fr takes exactly what the percent leaves.
    fixture f;
    f.items = {{}, {}};
    grid_style gs{};
    gs.columns = 2;
    gs.column[0] = {percent(50), 0.f};
    gs.column[1] = {auto_length, 1.f};
    std::vector<lx::rect2f> rects;
    auto r = run(f, gs, definite_input(200, 50), rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[0].width == 100);
    LUMEN_CHECK(rects[1].x == 100 && rects[1].width == 100);
}

LUMEN_TEST(grid_auto_tracks_size_to_their_items) {
    // No template: both tracks auto — each is its widest item; the container is the sum.
    fixture f;
    f.items = {{}, {40.f, 12.f}};
    grid_style gs{};
    gs.columns = 2;
    std::vector<lx::rect2f> rects;
    auto r = run(f, gs, auto_input(), rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[0].width == 10 && rects[1].width == 40);
    LUMEN_CHECK(r->width == 50);
    LUMEN_CHECK(r->height == 12);
}

LUMEN_TEST(grid_spans_and_explicit_placement) {
    // A two-column span takes row 0's whole width; the next items auto-place into row 1.
    fixture f;
    f.items = {{}, {}, {}};
    f.placements = {grid_placement{.column = 1, .row = 0, .column_span = 2, .row_span = 1},
                    {},
                    {}};
    grid_style gs{};
    gs.columns = 2;
    gs.column[0] = {px(80), 0.f};
    gs.column[1] = {px(80), 0.f};
    std::vector<lx::rect2f> rects;
    auto r = run(f, gs, definite_input(160, 100), rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[0].width == 160 && rects[0].y == 0);
    LUMEN_CHECK(rects[1].y == 10 && rects[1].x == 0);
    LUMEN_CHECK(rects[2].x == 80 && rects[2].y == 10);
}

LUMEN_TEST(grid_explicit_column_with_auto_row_takes_the_first_free_row) {
    // Column 2 row 0 is taken by item 0, so the explicitly placed item 1 goes to row 1's
    // column 2; auto placement continues from its own cursor (row 1, column 1).
    fixture f;
    f.items = {{}, {}, {}};
    f.placements = {{}, grid_placement{.column = 2, .row = 0}, {}};
    grid_style gs{};
    gs.columns = 2;
    std::vector<lx::rect2f> rects;
    auto r = run(f, gs, auto_input(), rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[1].x == 10 && rects[1].y == 0);
    LUMEN_CHECK(rects[2].x == 0 && rects[2].y == 10);
}

LUMEN_TEST(grid_fr_row_takes_the_definite_height) {
    // One fr row on a definite 80 px height: the row is 80 (the items' 10 is only the
    // content base an fr row grows past).
    fixture f;
    f.items = {{}, {}};
    grid_style gs{};
    gs.columns = 2;
    gs.rows = 1;
    gs.row[0] = {auto_length, 1.f};
    std::vector<lx::rect2f> rects;
    auto r = run(f, gs, definite_input(100, 80), rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[0].height == 80);
    LUMEN_CHECK(r->height == 80);
}

LUMEN_TEST(grid_padding_offsets_the_tracks) {
    // 7 px left and 5 px top padding: the item sits inside; the definite size includes it.
    fixture f;
    f.items = {{}};
    grid_style gs{};
    gs.columns = 1;
    gs.column[0] = {px(50), 0.f};
    gs.padding = {px(5), px(7), px(5), px(7)};
    std::vector<lx::rect2f> rects;
    auto r = run(f, gs, definite_input(64, 20), rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[0].x == 7 && rects[0].y == 5 && rects[0].width == 50);
    LUMEN_CHECK(r->width == 64 && r->height == 20);
}

LUMEN_TEST(grid_min_content_solve_runs) {
    // The min hint reaches the items' measurement (the fixture cannot wrap; the solve
    // stays consistent with the same sizes).
    fixture f;
    f.items = {{100.f, 10.f}};
    grid_style gs{};
    gs.columns = 1;
    std::vector<lx::rect2f> rects;
    grid_input in = auto_input();
    in.min_content_width = true;
    auto r = run(f, gs, in, rects);
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(rects[0].width == 100 && r->width == 100);
}

LUMEN_TEST(grid_container_node_measures_and_arranges) {
    // A real grid_node through ui_root: 100+100 px columns, item b placed at column 2
    // row 2 (x = 100, y = 40). The container measures to its tracks.
    ui_harness h{&describe_two_cells_t::build, {0, 0, 400, 300}};
    const auto size = h.node({}).measure({{}, {400, 400}}, lx::ui::size_hint::preferred);
    LUMEN_CHECK(size.width == 400); // a definite container size wins over the tracks
    LUMEN_CHECK(same(h.node({}).bounds(), {0, 0, 400, 300}));
    LUMEN_CHECK(same(h.node({0}).bounds(), {0, 0, 100, 40}));
    LUMEN_CHECK(same(h.node({1}).bounds(), {100, 40, 100, 40}));
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
