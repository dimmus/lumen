module;

import lx.foundation;
import lx.wayland.server;

export module lx.compositor:protocol_managers;

import :surface;

export namespace lx::compositor {

enum class layer_shell_layer { background, bottom, top, overlay };
enum class decoration_mode { client, server };

struct layer_surface_state {
    lx::surface_id surface{};
    layer_shell_layer layer = layer_shell_layer::top;
    lx::output_id output{};
    lx::rect2i geometry{};
    bool mapped = false;
};

struct decoration_state {
    lx::toplevel_id toplevel{};
    decoration_mode mode = decoration_mode::client;
    bool negotiated = false;
};

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

struct session_lock_state {
    bool active = false;
    lx::surface_id lock_surface{};
};

struct text_input_state {
    lx::surface_id focused_surface{};
    const char* preedit = "";
    const char* commit_text = "";
};

struct color_management_state {
    lx::image_description_id surface_desc{};
    lx::image_description_id output_desc{};
    lx::color_space compositing_space = lx::color_space::scrgb;
};

/// Registers and tracks standard public Wayland globals (layer-shell, decoration, workspace, …).
class protocol_managers {
public:
    [[nodiscard]] lx::result<void> install(wayland::server& server);

    // ext-session-lock-v1
    void set_session_locked(bool locked);
    [[nodiscard]] bool session_locked() const;
    void set_lock_surface(lx::surface_id surface);
    [[nodiscard]] lx::surface_id lock_surface() const;

    // wp-fractional-scale-v1
    void set_fractional_scale(unsigned output_index, float scale);
    [[nodiscard]] float fractional_scale(unsigned output_index) const;

    // wlr-layer-shell-v1
    [[nodiscard]] lx::result<layer_surface_state> map_layer_surface(lx::surface_id surface,
                                                                     layer_shell_layer layer);
    void unmap_layer_surface(lx::surface_id surface);

    // xdg-decoration-v1
    void negotiate_decoration(lx::toplevel_id toplevel, decoration_mode mode);
    [[nodiscard]] decoration_mode decoration_for(lx::toplevel_id toplevel) const;

    // ext-workspace-v1 (read path)
    void set_workspaces(const workspace_entry* entries, unsigned count);
    [[nodiscard]] unsigned workspace_count() const;
    [[nodiscard]] workspace_entry workspace(unsigned index) const;

    // wlr-output-management-v1
    void apply_output_configuration(const output_config_head* heads, unsigned count);
    [[nodiscard]] unsigned output_config_count() const;
    [[nodiscard]] output_config_head output_config(unsigned index) const;

    // text-input-v3 / input-method-v2
    void set_text_input_focus(lx::surface_id surface);
    [[nodiscard]] const text_input_state& text_input() const;

    // pointer / selection / latency / color (state tracked for shell + render)
    void set_pointer_locked(lx::surface_id surface, bool locked);
    void set_primary_selection(const char* utf8);
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
    session_lock_state session_lock_{};
    float output_scales_[16]{};
    layer_surface_state layers_[32]{};
    unsigned layer_count_ = 0;
    decoration_state decorations_[64]{};
    unsigned decoration_count_ = 0;
    workspace_entry workspaces_[16]{};
    unsigned workspace_count_ = 0;
    output_config_head output_configs_[16]{};
    unsigned output_config_count_ = 0;
    text_input_state text_input_{};
    lx::surface_id pointer_locked_surface_{};
    const char* primary_selection_ = "";
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
        {"zwlr_layer_shell_v1", 4, false},
        {"zxdg_decoration_manager_v1", 1, false},
        {"ext_workspace_manager_v1", 1, false},
        {"zwlr_output_manager_v1", 4, false},
        {"ext_session_lock_manager_v1", 1, false},
        {"ext_foreign_toplevel_list_v1", 1, false},
        {"zwp_text_input_manager_v3", 1, false},
        {"zwp_input_method_manager_v2", 1, false},
        {"zwp_pointer_constraints_v1", 1, false},
        {"zwp_relative_pointer_manager_v1", 1, false},
        {"wp_fractional_scale_manager_v1", 1, false},
        {"wp_cursor_shape_manager_v1", 1, false},
        {"zwp_primary_selection_device_manager_v1", 1, false},
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
    for (unsigned i = 0; i < 16; ++i)
        output_scales_[i] = 1.f;
    return {};
}

void lx::compositor::protocol_managers::set_session_locked(bool locked) {
    session_lock_.active = locked;
}

bool lx::compositor::protocol_managers::session_locked() const { return session_lock_.active; }

void lx::compositor::protocol_managers::set_lock_surface(lx::surface_id surface) {
    session_lock_.lock_surface = surface;
}

lx::surface_id lx::compositor::protocol_managers::lock_surface() const {
    return session_lock_.lock_surface;
}

void lx::compositor::protocol_managers::set_fractional_scale(unsigned output_index, float scale) {
    if (output_index < 16)
        output_scales_[output_index] = scale;
}

float lx::compositor::protocol_managers::fractional_scale(unsigned output_index) const {
    return output_index < 16 ? output_scales_[output_index] : 1.f;
}

lx::result<lx::compositor::layer_surface_state>
lx::compositor::protocol_managers::map_layer_surface(lx::surface_id surface,
                                                     layer_shell_layer layer) {
    if (layer_count_ >= 32)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "layer surface table full");
    auto& entry = layers_[layer_count_++];
    entry.surface = surface;
    entry.layer = layer;
    entry.mapped = true;
    return entry;
}

void lx::compositor::protocol_managers::unmap_layer_surface(lx::surface_id surface) {
    for (unsigned i = 0; i < layer_count_; ++i) {
        if (layers_[i].surface == surface) {
            layers_[i] = layers_[--layer_count_];
            return;
        }
    }
}

void lx::compositor::protocol_managers::negotiate_decoration(lx::toplevel_id toplevel,
                                                             decoration_mode mode) {
    for (unsigned i = 0; i < decoration_count_; ++i) {
        if (decorations_[i].toplevel == toplevel) {
            decorations_[i].mode = mode;
            decorations_[i].negotiated = true;
            return;
        }
    }
    if (decoration_count_ < 64) {
        decorations_[decoration_count_++] = {toplevel, mode, true};
    }
}

lx::compositor::decoration_mode
lx::compositor::protocol_managers::decoration_for(lx::toplevel_id toplevel) const {
    for (unsigned i = 0; i < decoration_count_; ++i)
        if (decorations_[i].toplevel == toplevel) return decorations_[i].mode;
    return decoration_mode::client;
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

void lx::compositor::protocol_managers::set_text_input_focus(lx::surface_id surface) {
    text_input_.focused_surface = surface;
}

const lx::compositor::text_input_state& lx::compositor::protocol_managers::text_input() const {
    return text_input_;
}

void lx::compositor::protocol_managers::set_pointer_locked(lx::surface_id surface, bool locked) {
    pointer_locked_surface_ = locked ? surface : lx::surface_id{};
}

void lx::compositor::protocol_managers::set_primary_selection(const char* utf8) {
    primary_selection_ = utf8 ? utf8 : "";
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
