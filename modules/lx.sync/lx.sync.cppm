module;

export module lx.sync;

export import :queue;

export namespace lx::sync {

/// Memory ordering helpers for cross-thread handoff documentation.
enum class handoff {
    ui_to_render,   ///< scene snapshot publish
    render_to_ui,   ///< presentation feedback (rare)
    worker_to_ui,   ///< async job completion
    wayland_to_ui,  ///< client buffer commit notification
};

} // namespace lx::sync
