module;

import lx.foundation;
import lx.wayland.server;

export module lx.compositor:protocol_managers;

import :surface;

export namespace lx::compositor {

struct workspace_entry {
    lx::workspace_id id{};
    const char* name = "";
    bool active = false;
};

struct output_config_head {
    lx::output_id output{};
    lx::rect2i geometry{};
    float scale = 1.f;
    bool enabled = true;
};

struct color_management_state {
    lx::image_description_id surface_desc{};
    lx::image_description_id output_desc{};
    lx::color_space compositing_space = lx::color_space::scrgb;
};

/// Registers and tracks standard public Wayland globals (workspace, …). xdg-decoration,
/// wlr-layer-shell, ext-session-lock and text-input/input-method are served by the protocol
/// wire units (`wire_*.cpp`; `:decoration`, `:layer_shell`, `:session_lock` and `:text_input`
/// hold their logic).
class protocol_managers {
public:
    [[nodiscard]] lx::result<void> install(wayland::server& server);

    // ext-workspace-v1 (read path)
    void set_workspaces(const workspace_entry* entries, unsigned count);
    [[nodiscard]] unsigned workspace_count() const;
    [[nodiscard]] workspace_entry workspace(unsigned index) const;

    // wlr-output-management-v1
    void apply_output_configuration(const output_config_head* heads, unsigned count);
    [[nodiscard]] unsigned output_config_count() const;
    [[nodiscard]] output_config_head output_config(unsigned index) const;

    // pointer / latency / color (state tracked for shell + render)
    void set_pointer_locked(lx::surface_id surface, bool locked);
    void set_surface_manager(surface_manager* surfaces);
    void set_content_type(lx::surface_id surface, content_hint hint);
    void set_tearing_allowed(lx::surface_id surface, bool allowed);
    void set_fifo_priority(lx::surface_id surface, bool enabled);
    [[nodiscard]] color_management_state color_state() const;
    void set_surface_image_description(lx::image_description_id desc);

    // xdg-session-management
    void restore_session_layout(const char* token);

private:
    wayland::server* server_ = nullptr;
    surface_manager* surfaces_ = nullptr;
    workspace_entry workspaces_[16]{};
    unsigned workspace_count_ = 0;
    output_config_head output_configs_[16]{};
    unsigned output_config_count_ = 0;
    lx::surface_id pointer_locked_surface_{};
    color_management_state color_{};
};

} // namespace lx::compositor


lx::result<void> lx::compositor::protocol_managers::install(wayland::server& server) {
    server_ = &server;

    struct global_spec {
        const char* name;
        unsigned version;
        bool privileged;
    };

    static constexpr global_spec k_public_globals[] = {
        {"ext_workspace_manager_v1", 1, false},
        {"zwlr_output_manager_v1", 4, false},
        {"ext_foreign_toplevel_list_v1", 1, false},
        {"zwp_pointer_constraints_v1", 1, false},
        {"zwp_relative_pointer_manager_v1", 1, false},
        {"wp_cursor_shape_manager_v1", 1, false},
        {"wp_single_pixel_buffer_manager_v1", 1, false},
        {"wp_alpha_modifier_v1", 1, false},
        {"wp_tearing_control_manager_v1", 1, false},
        {"wp_content_type_manager_v1", 1, false},
        {"wp_security_context_manager_v1", 1, false},
        {"wp_fifo_manager_v1", 1, false},
        {"wp_commit_timing_manager_v1", 1, false},
        {"wp_color_manager_v1", 1, false},
        {"ext_session_management_manager_v1", 1, false},
    };

    // Do not advertise globals without real bind handlers + wl_interface descriptors.
    // State tables below remain available for when handlers land (reassessment gate).
    (void)k_public_globals;
    return {};
}

void lx::compositor::protocol_managers::set_workspaces(const workspace_entry* entries,
                                                       unsigned count) {
    workspace_count_ = count < 16 ? count : 16;
    for (unsigned i = 0; i < workspace_count_; ++i)
        workspaces_[i] = entries[i];
}

unsigned lx::compositor::protocol_managers::workspace_count() const { return workspace_count_; }

lx::compositor::workspace_entry
lx::compositor::protocol_managers::workspace(unsigned index) const {
    return index < workspace_count_ ? workspaces_[index] : workspace_entry{};
}

void lx::compositor::protocol_managers::apply_output_configuration(const output_config_head* heads,
                                                                   unsigned count) {
    output_config_count_ = count < 16 ? count : 16;
    for (unsigned i = 0; i < output_config_count_; ++i)
        output_configs_[i] = heads[i];
}

unsigned lx::compositor::protocol_managers::output_config_count() const {
    return output_config_count_;
}

lx::compositor::output_config_head
lx::compositor::protocol_managers::output_config(unsigned index) const {
    return index < output_config_count_ ? output_configs_[index] : output_config_head{};
}

void lx::compositor::protocol_managers::set_pointer_locked(lx::surface_id surface, bool locked) {
    pointer_locked_surface_ = locked ? surface : lx::surface_id{};
}

void lx::compositor::protocol_managers::set_surface_manager(surface_manager* surfaces) {
    surfaces_ = surfaces;
}

void lx::compositor::protocol_managers::set_content_type(lx::surface_id surface,
                                                         content_hint hint) {
    if (surfaces_)
        surfaces_->set_content_hint(surface, hint);
}

void lx::compositor::protocol_managers::set_tearing_allowed(lx::surface_id, bool) {}

void lx::compositor::protocol_managers::set_fifo_priority(lx::surface_id, bool) {}

lx::compositor::color_management_state lx::compositor::protocol_managers::color_state() const {
    return color_;
}

void lx::compositor::protocol_managers::set_surface_image_description(lx::image_description_id desc) {
    color_.surface_desc = desc;
    color_.compositing_space = lx::color_space::scrgb;
}

void lx::compositor::protocol_managers::restore_session_layout(const char*) {}
