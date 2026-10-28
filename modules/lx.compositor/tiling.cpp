module;

// The master-stack layout's bodies (`:tiling`; why they live here: session_lock.cpp).

module lx.compositor;

import lx.foundation;
import :tiling;

import std;

namespace lx::compositor {

namespace {

/// Splits `column` (a rect) into `k` tiles stacked vertically, `gap` apart, into `out`.
void split_column(lx::rect2i column, unsigned k, int gap, lx::rect2i* out) {
    if (k == 0)
        return;
    const long long room =
        std::max<long long>(column.height - static_cast<long long>(gap) * (k - 1), k);
    for (unsigned i = 0; i < k; ++i) {
        const long long top = room * i / k;
        const long long bottom = room * (i + 1) / k;
        out[i] = {column.x, column.y + static_cast<int>(top) + gap * static_cast<int>(i),
                  std::max(column.width, 1), std::max(static_cast<int>(bottom - top), 1)};
    }
}

} // namespace

void master_stack_layout(lx::rect2i area, unsigned master_count, double ratio, int gaps,
                         std::span<lx::rect2i> out) {
    const auto n = static_cast<unsigned>(out.size());
    if (n == 0)
        return;
    gaps = std::max(gaps, 0);
    // The margin, kept only while it leaves something to lay out.
    lx::rect2i inner{area.x + gaps, area.y + gaps, area.width - 2 * gaps, area.height - 2 * gaps};
    if (inner.width < 1 || inner.height < 1)
        inner = area;
    const unsigned masters = std::min(master_count, n);
    const unsigned stack = n - masters;
    if (masters == 0 || stack == 0) {
        split_column(inner, n, gaps, out.data());
        return;
    }
    ratio = std::clamp(ratio, k_master_ratio_min, k_master_ratio_max);
    const int room = std::max(inner.width - gaps, 2);
    const int master_w = std::clamp(static_cast<int>(std::lround(room * ratio)), 1, room - 1);
    const lx::rect2i master{inner.x, inner.y, master_w, inner.height};
    const lx::rect2i rest{inner.x + master_w + gaps, inner.y, room - master_w, inner.height};
    split_column(master, masters, gaps, out.data());
    split_column(rest, stack, gaps, out.data() + masters);
}

const char* layout_name(workspace_layout l) {
    return l == workspace_layout::master_stack ? "master-stack" : "floating";
}

} // namespace lx::compositor
