module;

// lx.ui.host — lx.ui on Wayland surfaces (ADR-010 §7 Phase 5): Lumen's GDK / QtWaylandClient.
// One `context` per UI thread (the client_gpu, renderer, atlas, glyph and icon caches, theme,
// worker, timer and seat routing) drives any number of `surface`s, each an lx.ui root with its
// own ui_context, swapchain and frame loop, whatever its role (`:roles`: xdg_toplevel,
// zwlr_layer_surface_v1, xdg_popup on any of those, wl_subsurface).
//
// Layer 5 (Desktop): lx.app (windows), lx.shell (panel, launcher, switcher, notifications,
// menus) and lx.portal build on it. Native handles stay in lx.gfx and lx.wayland.client.

export module lx.ui.host;

export import :input;
export import :surface;
export import :context;
export import :roles;
