module;

import lx.foundation;
import lx.shell.policy;
import lx.wayland.client;

export module lx.shell:bridge;

export namespace lx::shell {

/// Client-side consumer of zlm_policy_bridge_v1 snapshot + delta events.
struct bridge_snapshot {
    unsigned workspace_count = 0;
    unsigned toplevel_count = 0;
    unsigned output_count = 0;
    bool complete = false;
};

using snapshot_handler = void (*)(bridge_snapshot snapshot, void* user_data);
using toplevel_created_handler = void (*)(lx::toplevel_id id, void* user_data);
using toplevel_destroyed_handler = void (*)(lx::toplevel_id id, void* user_data);
using focus_changed_handler = void (*)(lx::toplevel_id old_id, lx::toplevel_id new_id,
                                       void* user_data);
using geometry_handler = void (*)(lx::toplevel_id id, lx::rect2i bounds, void* user_data);
using state_handler = void (*)(lx::toplevel_id id, lx::shell::toplevel_state state,
                               void* user_data);

struct bridge_callbacks {
    snapshot_handler on_snapshot = nullptr;
    toplevel_created_handler on_toplevel_created = nullptr;
    toplevel_destroyed_handler on_toplevel_destroyed = nullptr;
    focus_changed_handler on_focus_changed = nullptr;
    geometry_handler on_geometry = nullptr;
    state_handler on_state = nullptr;
    void* user_data = nullptr;
};

class policy_bridge {
public:
    explicit policy_bridge(lx::wayland::display& display);

    /// Bind zlm_shell_v1 and acquire policy_bridge (P0).
    [[nodiscard]] lx::result<void> connect();

    void set_callbacks(bridge_callbacks callbacks);

    /// Shell → compositor policy requests.
    void request_activate(lx::toplevel_id id);
    void request_close(lx::toplevel_id id);
    void request_raise(lx::toplevel_id id);
    void request_lower(lx::toplevel_id id);
    void set_stacking_index(lx::toplevel_id id, unsigned index);
    void request_maximize(lx::toplevel_id id);
    void request_fullscreen(lx::toplevel_id id, lx::output_id output);
    void request_minimize(lx::toplevel_id id);

    [[nodiscard]] bool is_connected() const;
    [[nodiscard]] bridge_snapshot last_snapshot() const;

private:
    lx::wayland::display* display_ = nullptr;
    bridge_callbacks callbacks_{};
    bridge_snapshot last_snapshot_{};
    bool connected_ = false;
};

} // namespace lx::shell


lx::shell::policy_bridge::policy_bridge(lx::wayland::display& display) : display_{&display} {}

lx::result<void> lx::shell::policy_bridge::connect() {
    return lx::not_implemented("lx::shell::policy_bridge::connect");
}

void lx::shell::policy_bridge::set_callbacks(bridge_callbacks callbacks) {
    callbacks_ = callbacks;
}

void lx::shell::policy_bridge::request_activate(lx::toplevel_id id) { (void)id; }
void lx::shell::policy_bridge::request_close(lx::toplevel_id id) { (void)id; }
void lx::shell::policy_bridge::request_raise(lx::toplevel_id id) { (void)id; }
void lx::shell::policy_bridge::request_lower(lx::toplevel_id id) { (void)id; }
void lx::shell::policy_bridge::set_stacking_index(lx::toplevel_id id, unsigned index) {
    (void)id;
    (void)index;
}
void lx::shell::policy_bridge::request_maximize(lx::toplevel_id id) { (void)id; }
void lx::shell::policy_bridge::request_fullscreen(lx::toplevel_id id, lx::output_id output) {
    (void)id;
    (void)output;
}
void lx::shell::policy_bridge::request_minimize(lx::toplevel_id id) { (void)id; }

bool lx::shell::policy_bridge::is_connected() const { return connected_; }
lx::shell::bridge_snapshot lx::shell::policy_bridge::last_snapshot() const {
    return last_snapshot_;
}
