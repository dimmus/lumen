module;

// lx.wayland.client:layer — wlr-layer-shell's zwlr_layer_surface_v1: a surface the compositor
// places on one of four layers of an output (background, bottom, top, overlay), anchored to
// its edges, optionally reserving an exclusive zone — panels, docks, launchers, on-screen
// displays, wallpapers, notification stacks. Like the xdg roles: creating it commits nothing;
// set the state the first configure depends on, commit the surface without a buffer, draw
// for the configure, `ack_configure` its serial before the commit that shows it. A popup on a
// layer surface is `popup::create(…, layer_surface&, …)` (:xdg). Bodies in layer.cpp.
//
// Built with the protocol's glue (LUMEN_PROTOCOL_WLR_LAYER_SHELL_UNSTABLE_V1); without it
// `create` returns not_implemented and the rest of lx.wayland.client is unaffected.

struct zwlr_layer_surface_v1;

export module lx.wayland.client:layer;

import std;

import lx.foundation;
import :types;
import :display;
import :surface;

export namespace lx::wayland {

/// zwlr_layer_shell_v1.layer, bottom-most first.
enum class layer : std::uint32_t { background = 0, bottom = 1, top = 2, overlay = 3 };

/// zwlr_layer_surface_v1.keyboard_interactivity.
enum class keyboard_interactivity : std::uint32_t {
    /// Never focused (a panel that is only clicked, a wallpaper).
    none = 0,
    /// Takes the keyboard from everything while mapped on top / overlay (a lock-like launcher).
    exclusive = 1,
    /// Focused like a window: on a click, and (Lumen, sway) on map on top / overlay (v4).
    on_demand = 2,
};

/// zwlr_layer_surface_v1.anchor bits (also the exclusive-edge values, one bit each).
enum layer_anchor_bit : std::uint32_t {
    anchor_top = 1u << 0,
    anchor_bottom = 1u << 1,
    anchor_left = 1u << 2,
    anchor_right = 1u << 3,
};

struct layer_options {
    /// `display::output_at` slot to show it on; ~0u: the compositor picks (Lumen: the output
    /// under the pointer).
    unsigned output_slot = ~0u;
    layer level = layer::top;
    /// What it is for, for the compositor's rules ("panel", "launcher", "notifications").
    const char* name_space = "";
};

/// One zwlr_layer_surface_v1.configure.
struct layer_configure {
    std::uint32_t serial = 0;
    /// Logical; 0 on an axis: the client picks (the size it set on that axis).
    size2i size{};
    /// The first configure since `create`.
    bool initial = false;
};

class layer_handler {
public:
    virtual ~layer_handler() = default;
    virtual void on_configure(layer_surface& l, const layer_configure& c) = 0;
    /// zwlr_layer_surface_v1.closed: the compositor will not show it again (its output went
    /// away). The owner destroys it and may create another.
    virtual void on_closed(layer_surface& l) { (void)l; }
};

/// A zwlr_layer_surface_v1 on a surface. Non-movable (the listener holds its address).
/// Destroy its popups first, then it, then its surface.
/// Setters change double-buffered state the surface's next commit applies; a request the
/// bound version lacks is not sent and the setter returns false.
class layer_surface {
public:
    layer_surface() = default;
    ~layer_surface();
    layer_surface(const layer_surface&) = delete;
    layer_surface& operator=(const layer_surface&) = delete;

    /// zwlr_layer_shell_v1.get_layer_surface. `wayland_err::missing_global` without the
    /// layer shell, invalid_argument for an empty output slot or a surface not created.
    [[nodiscard]] lx::result<void> create(display& d, surface& s, const layer_options& options = {});
    void destroy();
    [[nodiscard]] bool valid() const { return layer_ != nullptr; }
    void set_handler(layer_handler* h) { handler_ = h; }
    [[nodiscard]] surface* target() const { return surface_; }
    /// The bound zwlr_layer_surface_v1 version (the layer shell's), 0 before `create`.
    [[nodiscard]] std::uint32_t version() const { return version_; }

    /// Logical; 0 on an axis: stretched between the two anchors of that axis.
    void set_size(size2i size);
    /// `layer_anchor_bit`s.
    void set_anchor(std::uint32_t anchors);
    /// > 0: reserve that many logical pixels from the anchored edge; 0: move aside for other
    /// zones; -1: ignore them (stretch over every zone).
    void set_exclusive_zone(std::int32_t zone);
    /// v5: the edge the zone applies to when the anchors do not say (one `layer_anchor_bit`,
    /// or 0 to deduce it again).
    bool set_exclusive_edge(std::uint32_t edge);
    void set_margin(std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left);
    /// `on_demand` needs v4.
    bool set_keyboard_interactivity(keyboard_interactivity k);
    /// v2: moves it to another layer.
    bool set_layer(layer l);

    /// Acks `serial` (the latest configure; a repeated or older one is refused and logged,
    /// not sent). Call before the commit that shows its result.
    void ack_configure(std::uint32_t serial);
    /// The last configure received, and the last serial acked.
    [[nodiscard]] const layer_configure& last_configure() const { return current_; }
    [[nodiscard]] std::uint32_t acked_serial() const { return acked_; }
    /// A configure arrived that is not acked yet (the latched one, `last_configure`).
    [[nodiscard]] bool configure_pending() const {
        return configures_ != 0 && current_.serial != acked_;
    }
    [[nodiscard]] bool configured() const { return configures_ != 0; }
    [[nodiscard]] unsigned configure_count() const { return configures_; }
    [[nodiscard]] bool closed() const { return closed_; }
    [[nodiscard]] void* native() const { return layer_; }

    // Event entry points.
    void on_configure(std::uint32_t serial, std::uint32_t width, std::uint32_t height);
    void on_closed();

private:
    display* display_ = nullptr;
    surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_ = nullptr;
    layer_handler* handler_ = nullptr;
    layer_configure current_{};
    std::uint32_t acked_ = 0;
    std::uint32_t version_ = 0;
    unsigned configures_ = 0;
    bool closed_ = false;
};

} // namespace lx::wayland
