module;

import lx.foundation;
import lx.runtime;
import lx.wayland.protocols;

export module lx.wayland.client;

export namespace lx::wayland {

class registry_listener {
public:
    virtual ~registry_listener() = default;
    virtual void on_global(const char* interface, unsigned name, int version) = 0;
    virtual void on_global_remove(unsigned name) = 0;
};

class display {
public:
    [[nodiscard]] static lx::result<display> connect(const char* name = nullptr);
    void disconnect();

    [[nodiscard]] int dispatch(int timeout_ms);
    void flush();
    void roundtrip();

    void set_registry_listener(registry_listener* listener);
};

class surface {
public:
    surface();
    ~surface();

    void attach(lx::buffer_id buffer, lx::point2i offset);
    void damage(lx::rect2i region);
    void commit();
    void set_frame_callback(void (*cb)(void*), void* data);

private:
    lx::surface_id id_{};
};

class xdg_toplevel {
public:
    explicit xdg_toplevel(surface& surface);

    void set_title(const char* title);
    void set_app_id(const char* app_id);
    void set_size(lx::size2i size);
    void set_maximized(bool maximized);
    void set_fullscreen(bool fullscreen);
};

} // namespace lx::wayland

module :private;

lx::result<lx::wayland::display> lx::wayland::display::connect(const char*) {
    return lx::not_implemented("lx::wayland::display::connect");
}
void lx::wayland::display::disconnect() {}
int lx::wayland::display::dispatch(int) { return 0; }
void lx::wayland::display::flush() {}
void lx::wayland::display::roundtrip() {}
void lx::wayland::display::set_registry_listener(registry_listener*) {}
lx::wayland::surface::surface() = default;
lx::wayland::surface::~surface() = default;
void lx::wayland::surface::attach(lx::buffer_id, lx::point2i) {}
void lx::wayland::surface::damage(lx::rect2i) {}
void lx::wayland::surface::commit() {}
void lx::wayland::surface::set_frame_callback(void (*)(void*), void*) {}
lx::wayland::xdg_toplevel::xdg_toplevel(surface&) {}
void lx::wayland::xdg_toplevel::set_title(const char*) {}
void lx::wayland::xdg_toplevel::set_app_id(const char*) {}
void lx::wayland::xdg_toplevel::set_size(lx::size2i) {}
void lx::wayland::xdg_toplevel::set_maximized(bool) {}
void lx::wayland::xdg_toplevel::set_fullscreen(bool) {}
