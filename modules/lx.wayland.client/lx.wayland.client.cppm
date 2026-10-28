module;

// lx.wayland.client — the client side of the wire: a thin libwayland-client wrapper with
// typed, non-movable RAII objects (destroy request on destruction), listeners bound to them
// through static thunks and the proxy's user data (no std::function, no allocation per
// event), and fixed tables for outputs, seats, feedback tranches and presentation feedback.
// It is a wrapper, not a toolkit: lx.app builds the window, the swapchain and the frame loop
// on it (ADR-010 §7 Phase 4). Platform layer: lx.foundation and lx.runtime only; no lx.gfx or
// lx.ui type, no Vulkan, EGL or GBM.
//
// Built with libwayland-client and the protocol glue (LUMEN_HAS_WAYLAND +
// LUMEN_HAS_PROTOCOL_GLUE with xdg-shell and the other protocols of protocols/manifest.toml);
// without them every create/connect returns not_implemented.

export module lx.wayland.client;

export import :types;
export import :dmabuf;
export import :seat;
export import :display;
export import :surface;
export import :xdg;
