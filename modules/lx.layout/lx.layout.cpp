module;

import lx.foundation;

module lx.layout;

lx::layout::layout_node::~layout_node() = default;

void lx::layout::layout_node::mark_dirty() { dirty_ = true; }
lx::rect2i lx::layout::layout_node::bounds() const { return bounds_; }

void lx::layout::flex_node::add(layout_node* child) {
    if (child_count_ < 64) children_[child_count_++] = child;
}

lx::size2i lx::layout::flex_node::measure(constraints c) {
    int main_max = 0;
    int cross_max = 0;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (!children_[i]) continue;
        const auto sz = children_[i]->measure(c);
        if (direction == axis::vertical) {
            main_max += sz.height + (i > 0 ? spacing : 0);
            cross_max = cross_max > sz.width ? cross_max : sz.width;
        } else {
            main_max += sz.width + (i > 0 ? spacing : 0);
            cross_max = cross_max > sz.height ? cross_max : sz.height;
        }
    }
    return direction == axis::vertical ? lx::size2i{cross_max, main_max}
                                       : lx::size2i{main_max, cross_max};
}

void lx::layout::flex_node::layout(lx::rect2i bounds) {
    bounds_ = bounds;
    int cursor = 0;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (!children_[i]) continue;
        lx::layout::constraints child_c{{0, 0}, {bounds.width, bounds.height}};
        const auto sz = children_[i]->measure(child_c);
        lx::rect2i child_bounds{};
        if (direction == axis::vertical) {
            child_bounds = {bounds.x, bounds.y + cursor, bounds.width, sz.height};
            cursor += sz.height + spacing;
        } else {
            child_bounds = {bounds.x + cursor, bounds.y, sz.width, bounds.height};
            cursor += sz.width + spacing;
        }
        children_[i]->layout(child_bounds);
    }
}

void lx::layout::grid_node::add(layout_node* child, int col_span) {
    if (child_count_ < 64) {
        children_[child_count_] = child;
        spans_[child_count_++] = static_cast<unsigned>(col_span);
    }
}

lx::size2i lx::layout::grid_node::measure(constraints c) {
    if (child_count_ == 0 || columns <= 0) return {};
    const int cols = columns;
    const int rows = static_cast<int>((child_count_ + static_cast<unsigned>(cols) - 1) /
                                        static_cast<unsigned>(cols));
    int max_w = 0;
    int max_h = 0;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (!children_[i]) continue;
        const auto sz = children_[i]->measure(c);
        max_w = max_w > sz.width ? max_w : sz.width;
        max_h = max_h > sz.height ? max_h : sz.height;
    }
    return {cols * max_w + column_spacing * (cols - 1),
            rows * max_h + row_spacing * (rows - 1)};
}

void lx::layout::grid_node::layout(lx::rect2i bounds) {
    bounds_ = bounds;
    if (columns <= 0) return;
    const int cell_w = bounds.width / columns;
    const int rows =
        static_cast<int>((child_count_ + static_cast<unsigned>(columns) - 1) /
                         static_cast<unsigned>(columns));
    const int cell_h = rows > 0 ? bounds.height / rows : bounds.height;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (!children_[i]) continue;
        const int col = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const int span = static_cast<int>(spans_[i]);
        const lx::rect2i cell{
            bounds.x + col * (cell_w + column_spacing),
            bounds.y + row * (cell_h + row_spacing),
            cell_w * span + column_spacing * (span - 1),
            cell_h};
        children_[i]->layout(cell);
    }
}

void lx::layout::layout_engine::solve(layout_node& root, constraints c) {
    (void)root.measure(c);
    root.layout({0, 0, c.max.width, c.max.height});
}

void lx::layout::layout_engine::incremental_solve(layout_node& dirty_root, constraints c) {
    solve(dirty_root, c);
}
