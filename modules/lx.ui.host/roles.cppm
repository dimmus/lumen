module;

// lx.ui.host:roles — what a `surface` is to the compositor (ADR-010 §7 Phase 5). Each role
// wraps lx.wayland.client's object, turns its configure into a `surface_configure` and acks
// it at the commit that shows it. This is Qt's QWaylandShellIntegration seam (what
// layer-shell-qt plugs into) rather than GTK 4's closed GdkToplevel / GdkPopup pair, which
// gtk4-layer-shell has to interpose on libwayland-client to extend.
//
// - toplevel_role: xdg_toplevel with a server-side decoration request; maximized, tiled,
//   fullscreen and resizing configures bound the size; xdg_toplevel.close → its handler.
// - layer_role: zwlr_layer_surface_v1; the configure's size, 0 on an axis the client's;
//   keyboard interactivity and the layer switchable live (committed at once); closed → its
//   handler.
// - popup_role: xdg_popup on a toplevel, popup or layer parent; its geometry's size is the
//   surface's; grab before `surface::show`; popup_done → its handler.
// - subsurface_role: wl_subsurface over a parent wl_surface, desynchronized by default (it
//   draws on its own frame callbacks); its size is the client's, configured at once.
//
// Create order: `surface::create`, the role's `create`, then `surface::show`. Destroy the
// role before the surface. UI affinity.

export module lx.ui.host:roles;

import std;

import lx.foundation;
import lx.wayland.client;

import :surface;

export namespace lx::ui::host {

class toplevel_role final : public surface_role, public lx::wayland::toplevel_handler {
public:
    toplevel_role() = default;
    ~toplevel_role() override;
    toplevel_role(const toplevel_role&) = delete;
    toplevel_role& operator=(const toplevel_role&) = delete;

    /// xdg_toplevel on `s` (created, not shown); the min size from `s`'s options.
    [[nodiscard]] lx::result<void> create(surface& s, const lx::wayland::toplevel_options& options);
    void destroy();
    [[nodiscard]] bool valid() const { return toplevel_.valid(); }
    /// xdg_toplevel.close: `fn(user)` (none: ignored).
    void set_close_handler(void (*fn)(void* user), void* user) {
        close_fn_ = fn;
        close_user_ = user;
    }
    [[nodiscard]] lx::wayland::toplevel& toplevel() { return toplevel_; }
    [[nodiscard]] const lx::wayland::toplevel& toplevel() const { return toplevel_; }

    void role_ack(std::uint32_t serial) override;
    void role_surface_destroyed() override { destroy(); }
    void on_configure(lx::wayland::toplevel& t, const lx::wayland::toplevel_configure& c) override;
    void on_close(lx::wayland::toplevel& t) override;

private:
    surface* surface_ = nullptr;
    lx::wayland::toplevel toplevel_{};
    void (*close_fn_)(void*) = nullptr;
    void* close_user_ = nullptr;
};

struct layer_role_options {
    lx::wayland::layer_options layer{};
    /// Logical px; 0 on an axis: stretched between its anchors (the compositor's size).
    lx::size2i size{};
    std::uint32_t anchor = 0;
    std::int32_t exclusive_zone = 0;
    /// top, right, bottom, left.
    std::int32_t margin[4]{};
    lx::wayland::keyboard_interactivity keyboard = lx::wayland::keyboard_interactivity::none;
};

class layer_role final : public surface_role, public lx::wayland::layer_handler {
public:
    layer_role() = default;
    ~layer_role() override;
    layer_role(const layer_role&) = delete;
    layer_role& operator=(const layer_role&) = delete;

    /// zwlr_layer_surface_v1 on `s` (created, not shown) with its initial state.
    [[nodiscard]] lx::result<void> create(surface& s, const layer_role_options& options);
    void destroy();
    [[nodiscard]] bool valid() const { return layer_.valid(); }
    /// zwlr_layer_surface_v1.closed: `fn(user)` (none: ignored; the surface stays unmapped).
    void set_closed_handler(void (*fn)(void* user), void* user) {
        closed_fn_ = fn;
        closed_user_ = user;
    }
    /// Live changes, committed at once (double-buffered layer state). False when the bound
    /// version lacks the request (nothing sent).
    bool set_keyboard_interactivity(lx::wayland::keyboard_interactivity k);
    bool set_layer(lx::wayland::layer l);
    void set_exclusive_zone(std::int32_t zone);
    [[nodiscard]] lx::wayland::layer_surface& layer() { return layer_; }
    [[nodiscard]] const lx::wayland::layer_surface& layer() const { return layer_; }

    void role_ack(std::uint32_t serial) override;
    void role_surface_destroyed() override { destroy(); }
    void on_configure(lx::wayland::layer_surface& l, const lx::wayland::layer_configure& c) override;
    void on_closed(lx::wayland::layer_surface& l) override;

private:
    surface* surface_ = nullptr;
    lx::wayland::layer_surface layer_{};
    void (*closed_fn_)(void*) = nullptr;
    void* closed_user_ = nullptr;
};

class popup_role final : public surface_role, public lx::wayland::popup_handler {
public:
    popup_role() = default;
    ~popup_role() override;
    popup_role(const popup_role&) = delete;
    popup_role& operator=(const popup_role&) = delete;

    /// xdg_popup on `s` (created, not shown), placed by `where` against the parent.
    [[nodiscard]] lx::result<void> create(surface& s, toplevel_role& parent,
                                          const lx::wayland::positioner_desc& where);
    [[nodiscard]] lx::result<void> create(surface& s, popup_role& parent,
                                          const lx::wayland::positioner_desc& where);
    [[nodiscard]] lx::result<void> create(surface& s, layer_role& parent,
                                          const lx::wayland::positioner_desc& where);
    void destroy();
    [[nodiscard]] bool valid() const { return popup_.valid(); }
    /// xdg_popup.popup_done: `fn(user)` (dismissed; destroy it).
    void set_done_handler(void (*fn)(void* user), void* user) {
        done_fn_ = fn;
        done_user_ = user;
    }
    /// An explicit grab for the press / key `serial` on the default seat; before `show`.
    void grab(std::uint32_t serial);
    void reposition(const lx::wayland::positioner_desc& where, std::uint32_t token);
    [[nodiscard]] lx::wayland::popup& popup() { return popup_; }
    [[nodiscard]] const lx::wayland::popup& popup() const { return popup_; }

    void role_ack(std::uint32_t serial) override;
    void role_surface_destroyed() override { destroy(); }
    void on_configure(lx::wayland::popup& p, const lx::wayland::popup_configure& c) override;
    void on_done(lx::wayland::popup& p) override;

private:
    [[nodiscard]] lx::result<void> created(surface& s, lx::result<void> made);

    surface* surface_ = nullptr;
    lx::wayland::popup popup_{};
    void (*done_fn_)(void*) = nullptr;
    void* done_user_ = nullptr;
};

class subsurface_role final : public surface_role {
public:
    subsurface_role() = default;
    ~subsurface_role();
    subsurface_role(const subsurface_role&) = delete;
    subsurface_role& operator=(const subsurface_role&) = delete;

    /// wl_subsurface of `s` over `parent` at `at` (logical px from the parent's origin),
    /// desynchronized, configured at once at `s`'s option size.
    [[nodiscard]] lx::result<void> create(surface& s, lx::wayland::surface& parent, lx::point2i at);
    void destroy();
    [[nodiscard]] bool valid() const { return sub_.valid(); }
    /// Double-buffered on the parent: applied by the parent's next commit.
    void set_position(lx::point2i at) { sub_.set_position(at); }
    void place_above(lx::wayland::surface& sibling) { sub_.place_above(sibling); }
    void place_below(lx::wayland::surface& sibling) { sub_.place_below(sibling); }
    /// A new size (logical px): drawn at the next frame.
    void resize(lx::size2i size);
    [[nodiscard]] lx::wayland::subsurface& subsurface() { return sub_; }

    void role_ack(std::uint32_t serial) override { (void)serial; }
    void role_surface_destroyed() override { destroy(); }

private:
    surface* surface_ = nullptr;
    lx::wayland::subsurface sub_{};
};

} // namespace lx::ui::host
