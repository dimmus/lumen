module;

import lx.foundation;
import lx.wayland.server;

export module lx.compositor:shell_bridge;

import :toplevel;

export namespace lx::compositor {

/// Server-side zlm_policy_bridge_v1 — emits snapshot on bind, deltas at runtime.
struct snapshot_workspace {
    lx::workspace_id workspace{};
    unsigned index = 0;
    const char* name = "";
    bool active = false;
};

struct snapshot_toplevel {
    lx::toplevel_id toplevel{};
    lx::workspace_id workspace{};
    const char* app_id = "";
    const char* title = "";
    lx::toplevel_state state = lx::toplevel_state::normal;
    lx::rect2i geometry{};
    unsigned stacking_index = 0;
};

struct snapshot_output {
    lx::output_id output{};
    const char* name = "";
    lx::rect2i geometry{};
    float scale = 1.f;
};

class shell_bridge {
public:
    explicit shell_bridge(wayland::server& server);

    /// Register zlm_shell_v1 global and policy_bridge factory (P0).
    [[nodiscard]] lx::result<void> install(const char* shell_binary_path);

    /// Emit full snapshot batch to a newly bound shell client.
    void emit_snapshot(lx::client_id shell_client, const toplevel_manager& toplevels);

    /// Runtime delta emitters (compositor → shell).
    void emit_toplevel_created(const toplevel_record& rec);
    void emit_toplevel_destroyed(lx::toplevel_id id);
    void emit_toplevel_state(lx::toplevel_id id, lx::toplevel_state state);
    void emit_toplevel_geometry(lx::toplevel_id id, lx::rect2i bounds);
    void emit_focus_changed(lx::toplevel_id old_id, lx::toplevel_id new_id);
    void emit_stacking_order_changed(lx::toplevel_id id, unsigned index, bool done);

    /// Handle shell → compositor policy requests (P0).
    void on_request_activate(lx::toplevel_id id);
    void on_request_close(lx::toplevel_id id);
    void on_set_stacking_index(lx::toplevel_id id, unsigned index);

    [[nodiscard]] bool is_shell_client(lx::client_id client) const;

private:
    wayland::server* server_ = nullptr;
    const char* shell_binary_path_ = "lumen-shell";
    lx::client_id shell_client_{};
};

} // namespace lx::compositor


lx::compositor::shell_bridge::shell_bridge(wayland::server& server) : server_{&server} {}

lx::result<void> lx::compositor::shell_bridge::install(const char* shell_binary_path) {
    shell_binary_path_ = shell_binary_path ? shell_binary_path : "lumen-shell";
    if (!server_) {
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "shell_bridge has no server");
    }
    server_->set_shell_binary_path(shell_binary_path_);
    // zlm_shell_v1 is advertised only once a real bind handler and wl_interface are wired
    // (protocol glue + privilege-gated trampoline). Until then, do not register a hollow global.
    return {};
}

void lx::compositor::shell_bridge::emit_snapshot(lx::client_id shell_client,
                                                 const toplevel_manager& toplevels) {
    shell_client_ = shell_client;
    (void)toplevels;
}

void lx::compositor::shell_bridge::emit_toplevel_created(const toplevel_record& rec) {
    (void)rec;
}

void lx::compositor::shell_bridge::emit_toplevel_destroyed(lx::toplevel_id id) { (void)id; }

void lx::compositor::shell_bridge::emit_toplevel_state(lx::toplevel_id id,
                                                       lx::toplevel_state state) {
    (void)id;
    (void)state;
}

void lx::compositor::shell_bridge::emit_toplevel_geometry(lx::toplevel_id id, lx::rect2i bounds) {
    (void)id;
    (void)bounds;
}

void lx::compositor::shell_bridge::emit_focus_changed(lx::toplevel_id old_id,
                                                      lx::toplevel_id new_id) {
    (void)old_id;
    (void)new_id;
}

void lx::compositor::shell_bridge::emit_stacking_order_changed(lx::toplevel_id id,
                                                               unsigned index, bool done) {
    (void)id;
    (void)index;
    (void)done;
}

void lx::compositor::shell_bridge::on_request_activate(lx::toplevel_id id) { (void)id; }
void lx::compositor::shell_bridge::on_request_close(lx::toplevel_id id) { (void)id; }
void lx::compositor::shell_bridge::on_set_stacking_index(lx::toplevel_id id, unsigned index) {
    (void)id;
    (void)index;
}

bool lx::compositor::shell_bridge::is_shell_client(lx::client_id client) const {
    return client == shell_client_;
}
