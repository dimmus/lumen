module;

import lx.foundation;
import lx.wayland.server;

export module lx.compositor:toplevel;

export namespace lx::compositor {

/// Compositor-owned registry: maps client xdg toplevels to zlm_toplevel_v1 handles.
struct toplevel_record {
    lx::toplevel_id id{};
    lx::client_id client{};
    lx::surface_id surface{};
    const char* app_id = "";
    const char* title = "";
    lx::toplevel_state state = lx::toplevel_state::normal;
    lx::rect2i geometry{};
    lx::workspace_id workspace{};
    unsigned stacking_index = 0;
    unsigned wire_id = 0; ///< zlm_toplevel_v1 Wayland object id (P0)
    bool override_redirect = false; ///< XWayland / OR windows — no decorations, fixed stacking
    lx::point2i global_origin{};  ///< Root coordinate space (multi-output / X11 bridge)
    unsigned net_wm_type = 0;     ///< _NET_WM_WINDOW_TYPE atom value (XWayland bridge)
    unsigned net_wm_state = 0;  ///< _NET_WM_STATE bitmask (fullscreen, maximized, …)
};

class toplevel_manager {
public:
    /// Called when an xdg toplevel maps — creates compositor-owned zlm handle.
    [[nodiscard]] lx::result<toplevel_record> on_xdg_map(lx::client_id client,
                                                         lx::surface_id surface,
                                                         const char* app_id);

    void on_xdg_unmap(lx::toplevel_id id);
    void on_xdg_title(lx::toplevel_id id, const char* title);
    void on_xdg_app_id(lx::toplevel_id id, const char* app_id);

    [[nodiscard]] const toplevel_record* find(lx::toplevel_id id) const;
    [[nodiscard]] const toplevel_record* find_by_surface(lx::surface_id surface) const;
    [[nodiscard]] unsigned count() const;

    void set_geometry(lx::toplevel_id id, lx::rect2i bounds);
    void set_state(lx::toplevel_id id, lx::toplevel_state state);
    void set_stacking_index(lx::toplevel_id id, unsigned index);
    void set_workspace(lx::toplevel_id id, lx::workspace_id workspace);
    void set_override_redirect(lx::toplevel_id id, bool value);
    void set_global_origin(lx::toplevel_id id, lx::point2i origin);
    void set_net_wm_type(lx::toplevel_id id, unsigned type_atom);
    void set_net_wm_state(lx::toplevel_id id, unsigned state_mask);

    /// Iterate all mapped toplevels (for snapshot emission).
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (unsigned i = 0; i < count_; ++i)
            fn(records_[i]);
    }

private:
    toplevel_record records_[256]{};
    unsigned count_ = 0;
    unsigned next_id_ = 1;
};

} // namespace lx::compositor


lx::result<lx::compositor::toplevel_record>
lx::compositor::toplevel_manager::on_xdg_map(lx::client_id client, lx::surface_id surface,
                                             const char* app_id) {
    if (count_ >= 256)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "toplevel table full");
    auto& rec = records_[count_++];
    rec.id = lx::toplevel_id{next_id_++};
    rec.client = client;
    rec.surface = surface;
    rec.app_id = app_id ? app_id : "";
    rec.wire_id = static_cast<unsigned>(rec.id.id());
    return rec;
}

void lx::compositor::toplevel_manager::on_xdg_unmap(lx::toplevel_id id) {
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].id == id) {
            records_[i] = records_[--count_];
            return;
        }
    }
}

void lx::compositor::toplevel_manager::on_xdg_title(lx::toplevel_id id, const char* title) {
    if (auto* rec = const_cast<toplevel_record*>(find(id)))
        rec->title = title ? title : "";
}

void lx::compositor::toplevel_manager::on_xdg_app_id(lx::toplevel_id id, const char* app_id) {
    if (auto* rec = const_cast<toplevel_record*>(find(id)))
        rec->app_id = app_id ? app_id : "";
}

const lx::compositor::toplevel_record*
lx::compositor::toplevel_manager::find(lx::toplevel_id id) const {
    for (unsigned i = 0; i < count_; ++i)
        if (records_[i].id == id) return &records_[i];
    return nullptr;
}

const lx::compositor::toplevel_record*
lx::compositor::toplevel_manager::find_by_surface(lx::surface_id surface) const {
    for (unsigned i = 0; i < count_; ++i)
        if (records_[i].surface == surface) return &records_[i];
    return nullptr;
}

unsigned lx::compositor::toplevel_manager::count() const { return count_; }

void lx::compositor::toplevel_manager::set_geometry(lx::toplevel_id id, lx::rect2i bounds) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->geometry = bounds;
}

void lx::compositor::toplevel_manager::set_state(lx::toplevel_id id,
                                                 lx::toplevel_state state) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->state = state;
}

void lx::compositor::toplevel_manager::set_stacking_index(lx::toplevel_id id, unsigned index) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->stacking_index = index;
}

void lx::compositor::toplevel_manager::set_workspace(lx::toplevel_id id,
                                                     lx::workspace_id workspace) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->workspace = workspace;
}

void lx::compositor::toplevel_manager::set_override_redirect(lx::toplevel_id id, bool value) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->override_redirect = value;
}

void lx::compositor::toplevel_manager::set_global_origin(lx::toplevel_id id, lx::point2i origin) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->global_origin = origin;
}

void lx::compositor::toplevel_manager::set_net_wm_type(lx::toplevel_id id, unsigned type_atom) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->net_wm_type = type_atom;
}

void lx::compositor::toplevel_manager::set_net_wm_state(lx::toplevel_id id, unsigned state_mask) {
    if (auto* rec = const_cast<toplevel_record*>(find(id))) rec->net_wm_state = state_mask;
}
