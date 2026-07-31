module;

import lx.foundation;
import lx.wayland.client;
import lx.wayland.protocols;

export module lx.shell.policy;

export namespace lx::shell {

using lx::toplevel_state;
using lx::placement;

class window_manager_policy {
public:
    virtual ~window_manager_policy() = default;
    virtual void on_toplevel_created(lx::toplevel_id id) = 0;
    virtual void on_toplevel_destroyed(lx::toplevel_id id) = 0;
    virtual void on_focus_changed(lx::toplevel_id old_id, lx::toplevel_id new_id) = 0;
    virtual void on_geometry(lx::toplevel_id id, lx::rect2i bounds) = 0;
    virtual void on_state(lx::toplevel_id id, toplevel_state state) = 0;
    virtual placement place_initial(lx::toplevel_id id, lx::size2i preferred) = 0;
};

class stacking_policy : public window_manager_policy {
public:
    void on_toplevel_created(lx::toplevel_id id) override;
    void on_toplevel_destroyed(lx::toplevel_id id) override;
    void on_focus_changed(lx::toplevel_id, lx::toplevel_id new_id) override;
    void on_geometry(lx::toplevel_id id, lx::rect2i bounds) override;
    void on_state(lx::toplevel_id id, toplevel_state state) override;
    placement place_initial(lx::toplevel_id id, lx::size2i preferred) override;

    void raise(lx::toplevel_id id);
    void lower(lx::toplevel_id id);
    void set_stacking_index(lx::toplevel_id id, unsigned index);

private:
    lx::toplevel_id stack_[256]{};
    unsigned count_ = 0;
};

class tiling_policy : public window_manager_policy {
public:
    void on_toplevel_created(lx::toplevel_id id) override;
    void on_toplevel_destroyed(lx::toplevel_id id) override;
    void on_focus_changed(lx::toplevel_id, lx::toplevel_id) override;
    void on_geometry(lx::toplevel_id id, lx::rect2i bounds) override;
    void on_state(lx::toplevel_id id, toplevel_state state) override;
    placement place_initial(lx::toplevel_id id, lx::size2i preferred) override;

    void toggle_tile(lx::toplevel_id id);
    void retile(lx::output_id output);
};

class policy_registry {
public:
    static policy_registry& instance();

    [[nodiscard]] window_manager_policy& active();
    void use_stacking();
    [[nodiscard]] bool load_tiling_plugin(const char* path);
    void use_tiling();

private:
    stacking_policy stacking_{};
    tiling_policy tiling_{};
    window_manager_policy* active_ = &stacking_;
};

} // namespace lx::shell

module :private;

void lx::shell::stacking_policy::on_toplevel_created(lx::toplevel_id) {}
void lx::shell::stacking_policy::on_toplevel_destroyed(lx::toplevel_id) {}
void lx::shell::stacking_policy::on_focus_changed(lx::toplevel_id, lx::toplevel_id) {}
void lx::shell::stacking_policy::on_geometry(lx::toplevel_id, lx::rect2i) {}
void lx::shell::stacking_policy::on_state(lx::toplevel_id, toplevel_state) {}
lx::placement lx::shell::stacking_policy::place_initial(lx::toplevel_id, lx::size2i p) {
    return {.size = p};
}
void lx::shell::stacking_policy::raise(lx::toplevel_id) {}
void lx::shell::stacking_policy::lower(lx::toplevel_id) {}
void lx::shell::stacking_policy::set_stacking_index(lx::toplevel_id, unsigned) {}

void lx::shell::tiling_policy::on_toplevel_created(lx::toplevel_id) {}
void lx::shell::tiling_policy::on_toplevel_destroyed(lx::toplevel_id) {}
void lx::shell::tiling_policy::on_focus_changed(lx::toplevel_id, lx::toplevel_id) {}
void lx::shell::tiling_policy::on_geometry(lx::toplevel_id, lx::rect2i) {}
void lx::shell::tiling_policy::on_state(lx::toplevel_id, toplevel_state) {}
lx::placement lx::shell::tiling_policy::place_initial(lx::toplevel_id, lx::size2i p) {
    return {.size = p, .state = toplevel_state::tiled};
}
void lx::shell::tiling_policy::toggle_tile(lx::toplevel_id) {}
void lx::shell::tiling_policy::retile(lx::output_id) {}

lx::shell::policy_registry& lx::shell::policy_registry::instance() {
    static policy_registry r;
    return r;
}
lx::shell::window_manager_policy& lx::shell::policy_registry::active() { return *active_; }
void lx::shell::policy_registry::use_stacking() { active_ = &stacking_; }
bool lx::shell::policy_registry::load_tiling_plugin(const char*) {
#if defined(LUMEN_ENABLE_TILING_PLUGIN) && LUMEN_ENABLE_TILING_PLUGIN
    return false;
#else
    (void)0;
    return false;
#endif
}
void lx::shell::policy_registry::use_tiling() { active_ = &tiling_; }
