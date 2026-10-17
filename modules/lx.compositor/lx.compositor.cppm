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
export import :surface_tree;
export import :toplevel;
export import :shell_bridge;
export import :xcursor;
export import :cursor;
export import :input_router;
export import :output;
export import :output_policy;
export import :output_management;
export import :toml_lite;
export import :output_config_store;
export import :buffer_lifecycle;
export import :protocol_managers;
export import :hotplug;
export import :protocols_p0;
export import :wm;
export import :positioner;
export import :popup;
export import :decoration;
export import :layer_shell;
export import :data_transfer;
export import :dnd;
export import :liveness;
export import :activation;
export import :keybindings;
export import :session_state;
export import :viewport;
export import :presentation_feedback;
export import :idle;
export import :display_power;
export import :session_lock;
export import :text_input;

export namespace lx::compositor {

/// Which composite path drives scanout.
///
/// `vulkan` is the production path: client dma-bufs are sampled by the GPU into an
/// exported scanout image. It is the wrong trade when the only Vulkan device is a software
/// rasterizer, because every frame then pays an upload, a tiling copy and a fullscreen
/// fragment shader to produce pixels the CPU already had. `cpu` composites straight into a
/// DRM dumb buffer instead — one pass over the frame, damage-limited.
enum class present_backend {
    /// Hardware Vulkan → `vulkan`; else hardware GL → `gl`; else `cpu`; no KMS → headless.
    automatic,
    vulkan,
    /// EGL + GLES on GBM. The path Mesa gives hardware acceleration on for drivers with no
    /// Vulkan driver — vmwgfx/SVGA3D, older radeon/nouveau, many SoCs.
    gl,
    cpu,
};

struct config {
    const char* socket_name = "lumen-0";
    /// The only executable allowed to bind zlm_shell_v1 (`--shell PATH`). A bare name is
    /// looked up next to the compositor's own binary, so the default is the lumen-shell
    /// installed or built alongside it; resolved to a canonical path at start.
    const char* shell_binary_path = "lumen-shell";
    bool enable_tiling = false;
    bool privileged_shell_only = true;
    double target_fps = 60.0;
    unsigned worker_thread_count = 1;
    present_backend present = present_backend::automatic;
    /// Advertise zxdg_decoration_manager_v1 and draw server-side frames for clients that do
    /// not insist on their own. False (`--decorations=client`) leaves every client to
    /// decorate itself.
    bool server_side_decorations = true;
    /// xdg_wm_base.ping timeout (`--ping-timeout MS`): a client that does not answer a ping
    /// this long is shown as not responding (title suffix, dimmed window, the shell's
    /// flag) until it answers. Mutter's default. 0 disables pinging.
    unsigned ping_timeout_ms = 5000;
    /// Output scale (`--scale`), for every output: 1.25 shows a 1366x768 mode as a
    /// 1093x614 logical desktop. Rounded to the nearest n/120 — the unit
    /// wp_fractional_scale_v1 speaks — and logged. Fractional-scale clients render at
    /// exactly this; others at the integer ceiling, downscaled. 0 (the default): sway's
    /// HiDPI rule per monitor (2 on a dense panel of 1200 rows or more, else 1).
    double output_scale = 0.0;
    /// Per-connector scales over `output_scale` (`--output-scale NAME=S`, repeatable):
    /// "NAME=S[,NAME=S...]".
    const char* output_scales = nullptr;
    /// Virtual outputs instead of the KMS display (`--virtual-outputs`, headless and
    /// multi-output protocol tests; no card is opened when set): "WxH[@S][,WxH[@S]...]",
    /// laid out left to right, each with its own frame clock. Null: the display's
    /// connectors, or one HEADLESS-1 without a display.
    const char* virtual_outputs = nullptr;
    /// Row-band threads for the CPU composite path, in addition to the render thread.
    ///
    /// Defaults to 0 on measurement, not principle: the fullscreen opaque case is a row
    /// memcpy, which one core already saturates memory bandwidth with, so bands only add
    /// dispatch latency (`bench_composite` shows this). Raise it on hosts with bandwidth
    /// to spare, or for scenes dominated by blended and format-converting draws, where it
    /// does pay.
    unsigned composite_thread_count = 0;
    scheduler::hot_path_budget hot_path{};
    scene::snapshot_config snapshot{
        .backpressure = scene::backpressure_policy::triple_buffer,
        .max_in_flight = 2,
    };
    gfx::import_cache_config import_cache{};
    runtime::memory_budget_config memory{};
};

namespace detail {
struct compositor_impl;
}

class seat_manager {
public:
    void set_focus(lx::surface_id surface);
    [[nodiscard]] lx::input::seat& seat();

    /// Binds to the compositor that owns the real seat. Until then `seat()` hands back an
    /// orphan instance so callers stay valid, and focus changes go nowhere.
    void bind(detail::compositor_impl* impl);

private:
    detail::compositor_impl* impl_ = nullptr;
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
    /// Debug aid: the next frame is repainted in full, read back and written as a PPM to
    /// $LUMEN_SCREENSHOT_DIR (else $XDG_RUNTIME_DIR) — GL path only. Async-signal-safe
    /// (lumen-compositor maps SIGUSR1 to it).
    void request_screenshot();

    [[nodiscard]] config configuration() const;
    [[nodiscard]] wayland::server& wayland();
    [[nodiscard]] surface_manager& surfaces();
    [[nodiscard]] toplevel_manager& toplevels();
    [[nodiscard]] window_manager& windows();
    [[nodiscard]] shell_bridge& shell_bridge();
    [[nodiscard]] cursor_manager& cursor();
    [[nodiscard]] scene::scene_graph& scene();
    [[nodiscard]] gfx::device& graphics();
    [[nodiscard]] gfx::dmabuf_import_cache& import_cache();
    [[nodiscard]] output_manager& outputs();
    /// xdg_wm_base.ping/pong liveness (tests read who is not responding).
    [[nodiscard]] liveness_tracker& liveness();
    [[nodiscard]] seat_manager& seats();
    /// Seat event delivery: focus, forwarded keys, the per-client input serials.
    [[nodiscard]] input_router& input();
    [[nodiscard]] scheduler::frame_scheduler& scheduler();
    [[nodiscard]] scheduler::budget_tracker& budget();
    [[nodiscard]] buffer_lifecycle_tracker& buffer_lifecycle();
    [[nodiscard]] runtime::memory_budget_coordinator& memory();
    [[nodiscard]] runtime::executor& executor();

    void tick_ui(scheduler::frame_tick tick);
    void tick_render();

    /// Virtual input for headless tests: each runs the same path a libinput event takes
    /// (window manager, router, serials, focus). UI affinity. A key updates the keymap
    /// state and the held keys first and sends the modifiers it changes after, exactly as
    /// the seat does; a motion puts the pointer at (`x`, `y`) in global coordinates.
    void inject_key(const lx::input::key_event& event);
    void inject_pointer_motion(const lx::input::pointer_motion_event& event);
    void inject_pointer_button(const lx::input::pointer_button_event& event);
    /// logind's Session.Unlock, as the signal delivers it (`loginctl unlock-session`): for
    /// headless tests. UI affinity.
    void inject_logind_unlock();
    /// A connector probe, as the hotplug monitor's would deliver it (ADR 008 §D.2): the
    /// diff, the policy, the output plans, the wl_output lifecycle and window migration run
    /// exactly as for a uevent. Without a KMS display the outputs it adds are virtual
    /// (composite-only), so headless tests reach every hotplug path. UI affinity.
    void simulate_connector_scan(const lx::drm::connector_scan& scan);
    /// Runs the work other threads posted to the UI thread (the event loop does this between
    /// dispatches; a test that drives `tick_ui` itself calls it). UI affinity.
    void run_posted_ui_tasks();
    /// The persisted output configuration as the compositor holds it (ADR 008 §C.5), and a
    /// save written now instead of after its 2 s debounce (tests). UI affinity.
    [[nodiscard]] const output_config& persisted_outputs() const;
    void save_output_config_now();
    /// The session lock's state (ext-session-lock-v1).
    [[nodiscard]] lock_state session_lock_state() const;

    [[nodiscard]] unsigned frame_index() const;
    [[nodiscard]] bool is_running() const;

private:
    friend struct detail::compositor_impl;
    detail::compositor_impl* impl_ = nullptr;
};

} // namespace lx::compositor
