module;

import lx.foundation;
import lx.shell.policy;
import lx.ui;
import lx.wayland.client;
import lx.runtime;
import lx.trace;

export module lx.shell;

export import :bridge;

export namespace lx::shell {

struct shell_config {
    const char* compositor_display = "lumen-0";
    const char* config_dir = nullptr;
    bool enable_tiling_plugin = false;
};

class workspace_controller {
public:
    void activate(lx::workspace_id id);
    void create(const char* name);
    void destroy(lx::workspace_id id);
    [[nodiscard]] lx::workspace_id active() const;
};

class panel {
public:
    void set_height(int px);
    void set_content(ui::widget* content);
    void show();
};

class shell_app {
public:
    explicit shell_app(shell_config config = {});
    [[nodiscard]] int run(int argc, char* argv[]);

    [[nodiscard]] workspace_controller& workspaces();
    [[nodiscard]] panel& primary_panel();
    [[nodiscard]] policy_registry& policies();
    [[nodiscard]] policy_bridge& bridge();

private:
    shell_config config_{};
    workspace_controller workspaces_{};
    panel panel_{};
    lx::wayland::display display_{};
    policy_bridge bridge_{display_};
    lx::runtime::event_loop loop_{};
};

} // namespace lx::shell

module :private;

lx::shell::shell_app::shell_app(shell_config config) : config_{config} {}
int lx::shell::shell_app::run(int, char*[]) {
    if (auto connected = bridge_.connect(); !connected)
        lx::trace::logger::global().log_error(connected.get_error(), "shell");
    return loop_.run();
}
lx::shell::workspace_controller& lx::shell::shell_app::workspaces() { return workspaces_; }
lx::shell::panel& lx::shell::shell_app::primary_panel() { return panel_; }
lx::shell::policy_registry& lx::shell::shell_app::policies() {
    return policy_registry::instance();
}
lx::shell::policy_bridge& lx::shell::shell_app::bridge() { return bridge_; }
void lx::shell::workspace_controller::activate(lx::workspace_id) {}
void lx::shell::workspace_controller::create(const char*) {}
void lx::shell::workspace_controller::destroy(lx::workspace_id) {}
lx::workspace_id lx::shell::workspace_controller::active() const { return {}; }
void lx::shell::panel::set_height(int) {}
void lx::shell::panel::set_content(ui::widget*) {}
void lx::shell::panel::show() {}
