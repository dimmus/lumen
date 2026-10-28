module;

// Master-stack tiling (ADR 012): the layout a workspace may use instead of stacking, as a
// pure function of the area and the window count. The window manager applies it
// (`window_manager::flush_tiling`); which window is where in the order lives in its records
// (`toplevel_record::tile_order`), the layout's parameters in the workspace's record. Pure and
// Wayland-free; no allocation. The bodies are in tiling.cpp (why: session_lock.cpp).

export module lx.compositor:tiling;

import lx.foundation;

import std;

export namespace lx::compositor {

/// How a workspace arranges its windows (T1): stacking, or dwm's `tile`.
enum class workspace_layout : unsigned char { floating, master_stack };

/// config.toml [tiling] (T8).
struct tiling_config {
    /// The layout every workspace starts with.
    workspace_layout layout = workspace_layout::floating;
    double master_ratio = 0.55;
    unsigned master_count = 1;
    /// Logical px around the tiles and between them.
    int gaps = 8;
};

inline constexpr double k_master_ratio_min = 0.1;
inline constexpr double k_master_ratio_max = 0.9;
inline constexpr double k_master_ratio_step = 0.05;
inline constexpr unsigned k_master_count_max = 16;

/// The frame rects of `out.size()` windows laid out master-stack in `area` (T1, T5): the
/// first `master_count` split the master column, which takes `ratio` of the width less the
/// gap between the columns; the rest split the stack column. With no stack windows the
/// masters take the whole width, with no masters the stack does. `gaps` is the margin and
/// the space between tiles; a column's height is split with no pixel lost. Every rect is at
/// least 1x1.
void master_stack_layout(lx::rect2i area, unsigned master_count, double ratio, int gaps,
                         std::span<lx::rect2i> out);

[[nodiscard]] const char* layout_name(workspace_layout l);

} // namespace lx::compositor
