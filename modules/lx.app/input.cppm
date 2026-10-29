module;

// lx.app:input — a window's input routing: lx.ui.host's `input_router` (wl_pointer frames,
// wl_keyboard keys through xkb, key repeat, wl_touch and the seat's zwp_text_input_v3 turned
// into the calls `lx::ui::ui_root` takes; lx.ui.host:input describes it), named here as lx.app
// has always named it.

export module lx.app:input;

import lx.ui.host;

export namespace lx::app {

using lx::ui::host::input_router;
using lx::ui::host::input_stats;
using lx::ui::host::k_wheel_lines;

} // namespace lx::app
