module;

// lx.wayland.client:xdg — xdg_surface roles: the toplevel (title, app id, size limits,
// maximise / fullscreen / minimise, interactive move / resize, the window menu, server-side
// decoration preferred) and the popup (a positioner, grab, reposition). Every configure
// sequence is latched and delivered whole at xdg_surface.configure; the owner acks the
// serial before the commit that shows it. Bodies in xdg.cpp.

struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;
struct zxdg_toplevel_decoration_v1;
struct wl_buffer;

export module lx.wayland.client:xdg;

import std;

import lx.foundation;
import :types;
import :display;
import :seat;
import :surface;

export namespace lx::wayland {

struct toplevel_options {
    const char* title = nullptr;
    const char* app_id = nullptr;
    /// The window's own taskbar icon (xdg_toplevel_icon_v1): `icon_size`² premultiplied
    /// 0xAARRGGBB words, read (copied into a shm pool) once during create; null: none.
    /// Square, 1..256 px. Quietly absent without the compositor's manager or the protocol
    /// glue; a failure to set it never fails the toplevel.
    const std::uint32_t* icon = nullptr;
    int icon_size = 0;
    /// Ask for server-side decorations (zxdg_decoration_manager_v1): the compositor draws
    /// the frame. Client-side when false or when the compositor has no decoration manager.
    bool server_side_decorations = true;
};

/// xdg_toplevel resize edges (xdg_toplevel.resize_edge).
enum class resize_edge : std::uint32_t {
    none = 0, top = 1, bottom = 2, left = 4, top_left = 5, bottom_left = 6, right = 8,
    top_right = 9, bottom_right = 10,
};

/// An xdg_toplevel on a surface. Non-movable. Creating it commits nothing: set what must
/// precede the first configure, then commit the surface without a buffer (the initial
/// commit) and draw once `on_configure` arrives.
class toplevel {
public:
    toplevel() = default;
    ~toplevel();
    toplevel(const toplevel&) = delete;
    toplevel& operator=(const toplevel&) = delete;

    [[nodiscard]] lx::result<void> create(display& d, surface& s,
                                          const toplevel_options& options = {});
    void destroy();
    [[nodiscard]] bool valid() const { return toplevel_ != nullptr; }
    void set_handler(toplevel_handler* h) { handler_ = h; }
    [[nodiscard]] surface* target() const { return surface_; }

    void set_title(const char* title);
    void set_app_id(const char* app_id);
    void set_min_size(size2i size);
    void set_max_size(size2i size);
    void set_maximized(bool on);
    /// `output_slot` ~0u: the compositor picks.
    void set_fullscreen(bool on, unsigned output_slot = ~0u);
    void set_minimized();
    void set_parent(toplevel* parent);
    /// The window geometry inside the surface (client-side shadows), logical.
    void set_window_geometry(rect2i r);
    /// Interactive move / resize / window menu, against a press serial on `s`.
    void move(seat& s, std::uint32_t serial);
    void resize(seat& s, std::uint32_t serial, resize_edge edges);
    void show_window_menu(seat& s, std::uint32_t serial, point2i at);
    /// zxdg_toplevel_decoration_v1.set_mode / unset_mode (no-op without the manager).
    void request_decoration(decoration_mode mode);

    /// Acks `serial` (the latest configure delivered; an older one is a protocol error the
    /// wrapper refuses and logs). Call before the commit that shows its result.
    void ack_configure(std::uint32_t serial);
    /// The last configure received, and the last serial acked.
    [[nodiscard]] const toplevel_configure& last_configure() const { return current_; }
    [[nodiscard]] std::uint32_t acked_serial() const { return acked_; }
    [[nodiscard]] bool configured() const { return configures_ != 0; }
    [[nodiscard]] unsigned configure_count() const { return configures_; }
    [[nodiscard]] decoration_mode decoration() const { return current_.decoration; }
    [[nodiscard]] void* native() const { return toplevel_; }
    [[nodiscard]] void* native_xdg_surface() const { return xdg_surface_; }

    // Event entry points.
    void on_toplevel_configure(std::int32_t w, std::int32_t h, const std::uint32_t* states,
                               std::size_t count);
    void on_bounds(std::int32_t w, std::int32_t h);
    void on_wm_capabilities(const std::uint32_t* caps, std::size_t count);
    void on_decoration(std::uint32_t mode);
    void on_surface_configure(std::uint32_t serial);
    void on_close();

private:
    /// The `icon` of `options` on this toplevel, over `d`'s manager. Fails soft: the
    /// caller logs once and the window goes without an icon.
    [[nodiscard]] lx::result<void> set_icon(display& d, const toplevel_options& options);

    display* display_ = nullptr;
    surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    zxdg_toplevel_decoration_v1* decoration_ = nullptr;
    /// The icon's pixels, kept for the toplevel's life: the protocol pins the buffer for
    /// as long as its icon object lives, and the set icon stays until the toplevel does.
    shm_pool icon_pool_;
    wl_buffer* icon_buffer_ = nullptr;
    toplevel_handler* handler_ = nullptr;
    toplevel_configure pending_{};
    toplevel_configure current_{};
    std::uint32_t acked_ = 0;
    unsigned configures_ = 0;
};

/// An xdg_popup on a surface, parented to a toplevel, another popup or a layer surface.
class popup {
public:
    popup() = default;
    ~popup();
    popup(const popup&) = delete;
    popup& operator=(const popup&) = delete;

    [[nodiscard]] lx::result<void> create(display& d, surface& s, toplevel& parent,
                                          const positioner_desc& where);
    [[nodiscard]] lx::result<void> create(display& d, surface& s, popup& parent,
                                          const positioner_desc& where);
    /// On a layer surface (a panel's menu): xdg_surface.get_popup without a parent, then
    /// zwlr_layer_surface_v1.get_popup before the initial commit. `where` is relative to the
    /// layer surface. not_implemented without wlr-layer-shell's glue.
    [[nodiscard]] lx::result<void> create(display& d, surface& s, layer_surface& parent,
                                          const positioner_desc& where);
    void destroy();
    [[nodiscard]] bool valid() const { return popup_ != nullptr; }
    void set_handler(popup_handler* h) { handler_ = h; }
    [[nodiscard]] surface* target() const { return surface_; }

    /// An explicit grab (menus), against a press serial; before the initial commit.
    void grab(seat& s, std::uint32_t serial);
    /// xdg_popup.reposition (v3); `token` comes back in the configure it causes.
    void reposition(const positioner_desc& where, std::uint32_t token);
    void ack_configure(std::uint32_t serial);
    [[nodiscard]] const popup_configure& last_configure() const { return current_; }
    [[nodiscard]] bool configured() const { return configures_ != 0; }
    [[nodiscard]] bool dismissed() const { return done_; }
    [[nodiscard]] void* native() const { return popup_; }
    [[nodiscard]] void* native_xdg_surface() const { return xdg_surface_; }

    void on_popup_configure(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h);
    void on_repositioned(std::uint32_t token);
    void on_surface_configure(std::uint32_t serial);
    void on_done();

private:
    /// `parent_xdg` null: a parent assigned by another protocol (layer-shell) next.
    [[nodiscard]] lx::result<void> create_on(display& d, surface& s, void* parent_xdg,
                                             const positioner_desc& where);

    display* display_ = nullptr;
    surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_popup* popup_ = nullptr;
    popup_handler* handler_ = nullptr;
    popup_configure pending_{};
    popup_configure current_{};
    unsigned configures_ = 0;
    bool done_ = false;
};

} // namespace lx::wayland
