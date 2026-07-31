module;

export module lx.foundation:wm_types;

import :types;
import :handles;

export namespace lx {

/// Shared WM vocabulary — used by compositor and shell.policy (no upward deps).
enum class toplevel_state { normal, maximized, fullscreen, minimized, tiled, modal };

struct placement {
    lx::point2i origin{};
    lx::size2i size{};
    toplevel_state state = toplevel_state::normal;
    lx::workspace_id workspace{};
    unsigned stacking_index = 0;
};

} // namespace lx
