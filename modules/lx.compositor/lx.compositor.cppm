module;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.scene;
import lx.scheduler;
import lx.input;
import lx.drm;
import lx.runtime;

export module lx.compositor;

export import :surface;
export import :toplevel;
export import :shell_bridge;
export import :cursor;
export import :output;
export import :buffer_lifecycle;
export import :protocol_managers;
export import :hotplug;
export import :protocols_p0;

export namespace lx::compositor {

struct config {
    const char* socket_name = "lumen-0";
    const char* shell_binary_path = "lumen-shell";
    bool enable_tiling = false;
    bool privileged_shell_only = true;
    double target_fps = 60.0;
    unsigned worker_thread_count = 1;
    scheduler::hot_path_budget hot_path{};
    scene::snapshot_config snapshot{
        .backpressure = scene::backpressure_policy::triple_buffer,
        .max_in_flight = 2,
    };
    gfx::import_cache_config import_cache{};
    runtime::memory_budget_config memory{};
};

class seat_manager {
public:
    void set_focus(lx::surface_id surface);
    [[nodiscard]] lx::input::seat& seat();
};

namespace detail {
struct compositor_impl;
}

class compositor {
public:
    explicit compositor(config cfg = {});
    ~compositor();

    compositor(const compositor&) = delete;
    compositor& operator=(const compositor&) = delete;

    [[nodiscard]] lx::result<void> start();
    [[nodiscard]] int run();
    void request_stop();

    [[nodiscard]] config configuration() const;
    [[nodiscard]] wayland::server& wayland();
    [[nodiscard]] surface_manager& surfaces();
    [[nodiscard]] toplevel_manager& toplevels();
    [[nodiscard]] shell_bridge& shell_bridge();
    [[nodiscard]] cursor_manager& cursor();
    [[nodiscard]] scene::scene_graph& scene();
    [[nodiscard]] gfx::device& graphics();
    [[nodiscard]] gfx::dmabuf_import_cache& import_cache();
    [[nodiscard]] output_manager& outputs();
    [[nodiscard]] seat_manager& seats();
    [[nodiscard]] scheduler::frame_scheduler& scheduler();
    [[nodiscard]] scheduler::presentation_tracker& presentation();
    [[nodiscard]] scheduler::budget_tracker& budget();
    [[nodiscard]] drm::kms_atomic_commit& kms_atomic();
    [[nodiscard]] buffer_lifecycle_tracker& buffer_lifecycle();
    [[nodiscard]] runtime::memory_budget_coordinator& memory();
    [[nodiscard]] runtime::executor& executor();

    void tick_ui(scheduler::frame_tick tick);
    void tick_render();

    [[nodiscard]] unsigned frame_index() const;
    [[nodiscard]] bool is_running() const;

private:
    friend struct detail::compositor_impl;
    detail::compositor_impl* impl_ = nullptr;
};

} // namespace lx::compositor
