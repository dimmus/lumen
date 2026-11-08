export module lx.layout:grid;

import :flex;

// lx.layout:grid — a CSS-grid subset for one container at a time, the sibling of `:flex`
// (ADR-010 §4.7): explicit column tracks × explicit or auto-flow rows, items placed in
// order (sparse, row-major) or at an explicit cell, spans supported, items stretched to
// their cell. Track sizes: px / rem / percent of the container's inner width (columns) or
// height (rows), `fr` shares of the space the fixed tracks leave (a definite inner size
// required — without one an `fr` track sizes to its items' max-content), and auto tracks,
// which size to the largest item placed in them. A row with no template is auto: its
// height is the largest item in it, measured at the cell width the columns give.
//
// The shape is `:flex`'s: one container per call, the children reached through callbacks,
// per-item and per-row state in caller-provided spans, no allocation. Placement is sparse,
// not dense: an auto-placed item goes in the first free cell at or after the cursor, and
// an explicitly placed one never moves an earlier item — the CSS Grid `sparse` packing.
// Whole-pixel concerns are the caller's (the rects are floats; lx.ui rounds edge by edge).

import lx.foundation;

import std;

export namespace lx::layout {

/// A grid track's sizing: `size` (px / rem / percent, or auto: the largest item placed in
/// the track), or `fr` shares of the space the fixed tracks leave (0: not flexible).
struct grid_track {
    length size = auto_length;
    float fr = 0.f;

    constexpr bool operator==(const grid_track&) const = default;
};

/// The most tracks one axis may declare.
inline constexpr unsigned k_grid_tracks = 16;
/// The most rows one solve places (an auto-flow grid makes at most one row per item).
inline constexpr unsigned k_grid_rows_max = 256;

/// Where an item sits: `column` / `row` are 1-based (0: auto-placed on that axis), the
/// spans ≥ 1. An explicit column with an auto row takes the first row whose cells in that
/// column are free.
struct grid_placement {
    std::uint8_t column = 0;
    std::uint8_t row = 0;
    std::uint8_t column_span = 1;
    std::uint8_t row_span = 1;

    constexpr bool operator==(const grid_placement&) const = default;
};

/// One grid container's style: the column tracks (`columns` of them, ≤ `k_grid_tracks`),
/// optional row tracks (`rows` of them; 0: every row is auto), the gaps, and the padding.
/// The padding box is what the tracks divide; items stretch across their whole cell (gaps
/// between cells, padding at the ends).
struct grid_style {
    std::uint8_t columns = 1;
    std::uint8_t rows = 0;
    grid_track column[k_grid_tracks]{};
    grid_track row[k_grid_tracks]{};
    length row_gap{};
    length column_gap{};
    edges padding{};

    constexpr bool operator==(const grid_style&) const = default;
};

/// The caller's view of one container's children: `placement` may return null
/// (auto-placed), `measure` answers a `child_query` with the child's border-box size —
/// `child_query` and `flex_size` are `:flex`'s, reused.
struct grid_children {
    void* context = nullptr;
    unsigned count = 0;
    const grid_placement* (*placement)(void* context, unsigned index) = nullptr;
    flex_size (*measure)(void* context, unsigned index, const child_query& query) = nullptr;
};

/// What the container is asked: `:flex`'s `flex_input`, reused.
using grid_input = flex_input;

struct grid_output {
    float width = 0.f;
    float height = 0.f;
    float content_width = 0.f;
    float content_height = 0.f;
    float inner_width = std::numeric_limits<float>::quiet_NaN();
    float inner_height = std::numeric_limits<float>::quiet_NaN();
};

/// Per-row resolved state: the row's size and its offset from the border-box origin.
struct grid_row_state {
    float size = 0.f;
    float offset = 0.f;
};

/// Per-item solver state, caller-owned (the scratch; `lx.ui` puts it on the stack).
struct grid_item_state {
    unsigned column = 0; ///< 0-based resolved column.
    unsigned row = 0;    ///< 0-based resolved row.
    unsigned col_span = 1;
    unsigned row_span = 1;
    flex_size size{};
};

/// Caller-provided scratch: one item state and one cell rect per child, and row state for
/// at least as many rows as the solve can produce — `k_grid_rows_max` of them, not the
/// child count: row spans, explicit `row` placements and template rows all make more rows
/// than items (a row span of 100 at the first item is 100 rows).
struct grid_scratch {
    std::span<grid_item_state> items;
    std::span<lx::rect2f> cells;
    std::span<grid_row_state> rows;
};

/// Lay out the children of one grid container. `out[i]` receives child `i`'s border box
/// relative to the container's border-box origin. Fails when a scratch span or `out` is
/// shorter than `children.count`, or a solve exceeds `k_grid_rows_max`; then nothing is
/// written. Allocates nothing.
[[nodiscard]] lx::result<grid_output> solve_grid(const grid_style& container,
                                                 const grid_input& input,
                                                 const grid_children& children,
                                                 std::span<lx::rect2f> out,
                                                 grid_scratch scratch);

} // namespace lx::layout

// ── implementation ──────────────────────────────────────────────────────────────

namespace {

using lx::layout::auto_length;
using lx::layout::child_query;
using lx::layout::flex_children;
using lx::layout::flex_size;
using lx::layout::grid_children;
using lx::layout::grid_input;
using lx::layout::grid_item_state;
using lx::layout::grid_output;
using lx::layout::grid_placement;
using lx::layout::grid_row_state;
using lx::layout::grid_scratch;
using lx::layout::grid_style;
using lx::layout::grid_track;
using lx::layout::k_grid_rows_max;
using lx::layout::k_grid_tracks;
using lx::layout::length;

constexpr float k_nan = std::numeric_limits<float>::quiet_NaN();
constexpr float k_inf = std::numeric_limits<float>::infinity();

[[nodiscard]] bool definite(float v) { return v == v; }

/// The occupancy bits: k_grid_rows_max rows × k_grid_tracks (≤ 64) columns.
using grid_occupied = std::uint64_t[k_grid_rows_max];

[[nodiscard]] bool cell_free(const grid_occupied& occ, unsigned r, unsigned c,
                             unsigned columns) {
    return c < columns && (occ[r] >> c & 1u) == 0;
}

void occupy(grid_occupied& occ, unsigned r, unsigned c) { occ[r] |= 1ull << c; }

} // namespace

lx::result<lx::layout::grid_output> lx::layout::solve_grid(
    const grid_style& cs, const grid_input& in, const grid_children& ch,
    std::span<lx::rect2f> out, grid_scratch scratch) {
    const unsigned n = ch.count;
    if (out.size() < n || scratch.cells.size() < n || scratch.items.size() < n)
        return lx::invalid_argument(
            "lx::layout::solve_grid: output or scratch shorter than the child count");
    // The rows this solve may write: `min(row_count, k_grid_rows_max)` — capped to what the
    // caller gave, so a solve never writes past its scratch (`lx.ui` passes 64). A
    // placement that would need a row past the cap fails instead, below.
    const unsigned row_cap =
        std::min<unsigned>(scratch.rows.size(), k_grid_rows_max);
    if (row_cap == 0 && n > 0)
        return lx::invalid_argument("lx::layout::solve_grid: no row state");
    if (cs.columns == 0 || cs.columns > k_grid_tracks)
        return lx::invalid_argument("lx::layout::solve_grid: columns out of range");

    const float rem = in.rem_px;
    const float pb_w = definite(in.parent_width) ? in.parent_width : 0.f;
    const float pb_h = definite(in.parent_height) ? in.parent_height : 0.f;
    float inner_w = definite(in.known_width)
                        ? in.known_width
                        : (definite(in.available_width) && !std::isinf(in.available_width)
                               ? in.available_width
                               : k_nan);
    const float pad_l = resolve(cs.padding.left, rem, pb_w);
    const float pad_r = resolve(cs.padding.right, rem, pb_w);
    const float pad_t = resolve(cs.padding.top, rem, pb_h);
    const float pad_b = resolve(cs.padding.bottom, rem, pb_h);
    const float col_gap = std::max(resolve(cs.column_gap, rem, definite(inner_w) ? inner_w : 0.f), 0.f);
    const float row_gap = std::max(resolve(cs.row_gap, rem, 0.f), 0.f);
    // The tracks divide the padding box; without a definite width they get the available
    // one (the content may overflow; the owner scrolls).
    const float track_base_w = definite(inner_w) ? inner_w - pad_l - pad_r : k_nan;

    // ── placement (sparse, row-major; the cursor only ever moves forward) ──
    grid_occupied occupied{};
    unsigned row_count = 0;
    unsigned cursor_row = 0, cursor_col = 0;
    for (unsigned i = 0; i < n; ++i) {
        const grid_placement* given = ch.placement ? ch.placement(ch.context, i) : nullptr;
        grid_item_state& it = scratch.items[i];
        it.col_span = std::clamp<unsigned>(given ? given->column_span : 1, 1, cs.columns);
        it.row_span = std::max<unsigned>(given ? given->row_span : 1, 1);
        it.col_span = std::min<unsigned>(it.col_span, k_grid_tracks);
        it.row_span = std::min<unsigned>(it.row_span, row_cap);

        const auto span_free = [&](unsigned r, unsigned c) {
            for (unsigned rr = r; rr < r + it.row_span; ++rr)
                for (unsigned cc = c; cc < c + it.col_span; ++cc)
                    if (!cell_free(occupied, rr, cc, cs.columns))
                        return false;
            return true;
        };
        const auto occupy_span = [&](unsigned r, unsigned c) {
            for (unsigned rr = r; rr < r + it.row_span; ++rr)
                for (unsigned cc = c; cc < c + it.col_span; ++cc)
                    occupy(occupied, rr, cc);
            row_count = std::max(row_count, r + it.row_span);
        };

        if (given && given->column > 0) {
            // An explicit column: the first row whose cells in it are free.
            it.column = std::min<unsigned>(given->column - 1, cs.columns - it.col_span);
            it.row = given->row > 0 ? given->row - 1 : 0;
            while (it.row + it.row_span <= row_cap && !span_free(it.row, it.column))
                ++it.row;
            if (it.row + it.row_span > row_cap)
                return lx::invalid_argument("lx::layout::solve_grid: too many rows");
        } else {
            it.column = cursor_col;
            it.row = cursor_row;
            while (it.column + it.col_span > cs.columns) {
                ++it.row;
                it.column = 0;
            }
            while (it.row + it.row_span <= row_cap && !span_free(it.row, it.column)) {
                ++it.column;
                if (it.column + it.col_span > cs.columns) {
                    ++it.row;
                    it.column = 0;
                }
            }
            if (it.row + it.row_span > row_cap)
                return lx::invalid_argument("lx::layout::solve_grid: too many rows");
            cursor_row = it.row;
            cursor_col = it.column + it.col_span;
        }
        occupy_span(it.row, it.column);
    }
    row_count = std::max(row_count, std::min<unsigned>(cs.rows, k_grid_tracks));
    row_count = std::min(row_count, row_cap);

    // ── column tracks: fixed, then auto (max-content of their single-column items), then
    // spanned items grow theirs, then fr shares what is left ──
    float cols[k_grid_tracks] = {};
    bool col_content[k_grid_tracks] = {}; // auto or fr before it grows: content-sized base
    for (unsigned c = 0; c < cs.columns; ++c) {
        const grid_track& t = cs.column[c];
        if (t.fr > 0.f) {
            col_content[c] = true;
        } else {
            const float v = resolve(t.size, rem, track_base_w);
            if (definite(v))
                cols[c] = std::max(v, 0.f);
            else
                col_content[c] = true;
        }
    }
    if (n > 0) {
        const child_query q{.known_width = k_nan,
                            .known_height = k_nan,
                            .available_width = k_inf,
                            .available_height = k_inf,
                            .parent_width = definite(track_base_w) ? track_base_w : k_nan,
                            .parent_height = k_nan,
                            .min_content_width = in.min_content_width,
                            .content_size = true};
        for (unsigned i = 0; i < n; ++i) {
            grid_item_state& it = scratch.items[i];
            if (it.col_span == 1 && col_content[it.column])
                cols[it.column] = std::max(
                    cols[it.column], ch.measure(ch.context, i, q).width);
        }
        // A spanned item grows its columns to fit it (an even split, one pass).
        for (unsigned i = 0; i < n; ++i) {
            const grid_item_state& it = scratch.items[i];
            if (it.col_span <= 1)
                continue;
            float spanned = -col_gap * static_cast<float>(it.col_span - 1);
            for (unsigned c = it.column; c < it.column + it.col_span; ++c)
                spanned += cols[c];
            const float need = ch.measure(ch.context, i, q).width - spanned;
            if (need > 0.f) {
                const float share = need / static_cast<float>(it.col_span);
                for (unsigned c = it.column; c < it.column + it.col_span; ++c)
                    cols[c] += share;
            }
        }
    }
    if (definite(track_base_w)) {
        // `1fr` = `minmax(auto, 1fr)`: the content base is the floor, the share of what
        // the FIXED tracks leave is the size.
        float fr_total = 0.f, fixed_used = col_gap * static_cast<float>(cs.columns - 1);
        for (unsigned c = 0; c < cs.columns; ++c) {
            if (cs.column[c].fr > 0.f)
                fr_total += cs.column[c].fr;
            else
                fixed_used += cols[c];
        }
        if (fr_total > 0.f) {
            const float free = std::max(track_base_w - fixed_used, 0.f);
            for (unsigned c = 0; c < cs.columns; ++c)
                if (cs.column[c].fr > 0.f)
                    cols[c] = std::max(cols[c], free * cs.column[c].fr / fr_total);
        }
    }

    // ── rows: items measured at their cell width; auto rows take the tallest; template
    // rows resolve against a definite inner height; fr rows share what is left ──
    float row_h[k_grid_rows_max] = {};
    for (unsigned i = 0; i < n; ++i) {
        grid_item_state& it = scratch.items[i];
        float cell_w = -col_gap * static_cast<float>(it.col_span - 1);
        for (unsigned c = it.column; c < it.column + it.col_span; ++c)
            cell_w += cols[c];
        const child_query q{.known_width = cell_w,
                            .known_height = k_nan,
                            .available_width = cell_w,
                            .available_height = k_inf,
                            .parent_width = definite(track_base_w) ? track_base_w : k_nan,
                            .parent_height = k_nan,
                            .content_size = true};
        it.size = ch.measure(ch.context, i, q);
        if (it.row_span == 1)
            row_h[it.row] = std::max(row_h[it.row], it.size.height);
    }
    const float inner_h =
        definite(in.known_height) ? in.known_height
        : (definite(in.available_height) && !std::isinf(in.available_height)
               ? in.available_height
               : k_nan);
    const float track_base_h = definite(inner_h) ? inner_h - pad_t - pad_b : k_nan;
    for (unsigned r = 0; r < std::min<unsigned>(cs.rows, k_grid_rows_max); ++r) {
        const grid_track& t = cs.row[r];
        if (t.fr > 0.f)
            continue;
        const float v = resolve(t.size, rem, track_base_h);
        if (definite(v))
            row_h[r] = std::max(row_h[r], std::max(v, 0.f));
    }
    float fr_total_h = 0.f, fixed_h = row_gap * static_cast<float>(row_count > 0 ? row_count - 1 : 0);
    for (unsigned r = 0; r < row_count; ++r) {
        if (r < cs.rows && cs.row[r].fr > 0.f)
            fr_total_h += cs.row[r].fr;
        else
            fixed_h += row_h[r];
    }
    if (fr_total_h > 0.f && definite(track_base_h)) {
        const float free = std::max(track_base_h - fixed_h, 0.f);
        for (unsigned r = 0; r < std::min<unsigned>(cs.rows, row_count); ++r)
            if (cs.row[r].fr > 0.f)
                row_h[r] = std::max(row_h[r], free * cs.row[r].fr / fr_total_h);
    }

    // ── offsets, item rects, sizes ──
    float col_off[k_grid_tracks] = {};
    for (unsigned c = 1; c < cs.columns; ++c)
        col_off[c] = col_off[c - 1] + cols[c - 1] + col_gap;
    float row_off[k_grid_rows_max] = {};
    for (unsigned r = 1; r < row_count; ++r)
        row_off[r] = row_off[r - 1] + row_h[r - 1] + row_gap;
    for (unsigned r = 0; r < row_count; ++r)
        scratch.rows[r] = {row_h[r], row_off[r] + pad_t};

    float content_w = 0.f, content_h = 0.f;
    for (unsigned i = 0; i < n; ++i) {
        const grid_item_state& it = scratch.items[i];
        const float x = pad_l + col_off[it.column];
        const float y = pad_t + row_off[it.row];
        float w = -col_gap * static_cast<float>(it.col_span - 1);
        for (unsigned c = it.column; c < it.column + it.col_span; ++c)
            w += cols[c];
        float h = -row_gap * static_cast<float>(it.row_span - 1);
        for (unsigned r = it.row; r < it.row + it.row_span; ++r)
            h += row_h[r];
        out[i] = {x, y, w, h};
        content_w = std::max(content_w, x - pad_l + w);
        content_h = std::max(content_h, y - pad_t + h);
    }

    const float tracks_w = col_off[cs.columns - 1] + cols[cs.columns - 1];
    float tracks_h = 0.f;
    if (row_count > 0)
        tracks_h = row_off[row_count - 1] + row_h[row_count - 1];
    const float total_w = definite(inner_w) ? inner_w
                          : definite(track_base_w) ? track_base_w
                                                   : tracks_w + pad_l + pad_r;
    const float total_h = definite(inner_h) ? inner_h : tracks_h + pad_t + pad_b;
    return grid_output{.width = total_w,
                       .height = total_h,
                       .content_width = content_w,
                       .content_height = content_h,
                       .inner_width = definite(inner_w) ? inner_w - pad_l - pad_r : k_nan,
                       .inner_height = definite(inner_h) ? inner_h - pad_t - pad_b : k_nan};
}
