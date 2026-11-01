module;

// Bodies of lx.wayland.client:layer — zwlr_layer_surface_v1. Gated on LX_WLC_LAYER (the
// protocol's glue), not on LX_WLC_REAL: without the glue every call is a not_implemented stub.

#include "client_glue.h"

module lx.wayland.client;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;

namespace lx::wayland {

namespace {
constexpr lx::trace::category log_layer{"wayland.client.layer"};
} // namespace

#if LX_WLC_LAYER

namespace {

void tag(void* proxy) { wl_proxy_set_tag(static_cast<wl_proxy*>(proxy), &lx_wlc_tag); }

const zwlr_layer_surface_v1_listener k_layer_listener = {
    .configure =
        [](void* data, zwlr_layer_surface_v1*, std::uint32_t serial, std::uint32_t w,
           std::uint32_t h) { static_cast<layer_surface*>(data)->on_configure(serial, w, h); },
    .closed = [](void* data, zwlr_layer_surface_v1*) { static_cast<layer_surface*>(data)->on_closed(); },
};

} // namespace

layer_surface::~layer_surface() { destroy(); }

lx::result<void> layer_surface::create(display& d, surface& s, const layer_options& options) {
    d.check_affinity();
    if (layer_)
        return lx::invalid_argument("lx::wayland::layer_surface::create: already created");
    auto* shell = static_cast<zwlr_layer_shell_v1*>(d.bind_on_demand(global_kind::layer_shell));
    if (!shell)
        return lx::make_error(lx::wayland_err::missing_global,
                              "zwlr_layer_shell_v1 is not available");
    if (!s.valid())
        return lx::invalid_argument("lx::wayland::layer_surface::create: no surface");
    if (static_cast<std::uint32_t>(options.level) > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)
        return lx::invalid_argument("lx::wayland::layer_surface::create: no such layer");
    wl_output* where = nullptr;
    if (options.output_slot != ~0u) {
        const output* o = d.output_at(options.output_slot);
        if (!o)
            return lx::invalid_argument("lx::wayland::layer_surface::create: no output in that slot");
        where = static_cast<wl_output*>(o->native());
    }
    layer_ = zwlr_layer_shell_v1_get_layer_surface(
        shell, static_cast<wl_surface*>(s.native()), where,
        static_cast<std::uint32_t>(options.level), options.name_space ? options.name_space : "");
    if (!layer_)
        return lx::make_error(lx::wayland_err::object_failed, "get_layer_surface failed");
    tag(layer_);
    zwlr_layer_surface_v1_add_listener(layer_, &k_layer_listener, this);
    display_ = &d;
    surface_ = &s;
    version_ = zwlr_layer_surface_v1_get_version(layer_);
    current_ = {};
    acked_ = 0;
    configures_ = 0;
    closed_ = false;
    return {};
}

void layer_surface::destroy() {
    if (layer_)
        zwlr_layer_surface_v1_destroy(layer_);
    layer_ = nullptr;
    surface_ = nullptr;
}

void layer_surface::set_size(size2i size) {
    if (layer_)
        zwlr_layer_surface_v1_set_size(layer_, static_cast<std::uint32_t>(std::max(0, size.width)),
                                       static_cast<std::uint32_t>(std::max(0, size.height)));
}

void layer_surface::set_anchor(std::uint32_t anchors) {
    // Bits past the four edges are a protocol error (invalid_anchor): not sent.
    if (layer_)
        zwlr_layer_surface_v1_set_anchor(layer_, anchors & 0xFu);
}

void layer_surface::set_exclusive_zone(std::int32_t zone) {
    if (layer_)
        zwlr_layer_surface_v1_set_exclusive_zone(layer_, zone);
}

bool layer_surface::set_exclusive_edge(std::uint32_t edge) {
    if (!layer_ || version_ < ZWLR_LAYER_SURFACE_V1_SET_EXCLUSIVE_EDGE_SINCE_VERSION)
        return false;
    if (edge > ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT || (edge & (edge - 1)) != 0) {
        log_layer.warn("set_exclusive_edge({:#x}) refused: not a single edge", edge);
        return false;
    }
    zwlr_layer_surface_v1_set_exclusive_edge(layer_, edge);
    return true;
}

void layer_surface::set_margin(std::int32_t top, std::int32_t right, std::int32_t bottom,
                               std::int32_t left) {
    if (layer_)
        zwlr_layer_surface_v1_set_margin(layer_, top, right, bottom, left);
}

bool layer_surface::set_keyboard_interactivity(keyboard_interactivity k) {
    const auto v = static_cast<std::uint32_t>(k);
    if (!layer_ || v > ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND ||
        (k == keyboard_interactivity::on_demand &&
         version_ < ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION))
        return false;
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_, v);
    return true;
}

bool layer_surface::set_layer(layer l) {
    const auto v = static_cast<std::uint32_t>(l);
    if (!layer_ || v > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ||
        version_ < ZWLR_LAYER_SURFACE_V1_SET_LAYER_SINCE_VERSION)
        return false;
    zwlr_layer_surface_v1_set_layer(layer_, v);
    return true;
}

void layer_surface::ack_configure(std::uint32_t serial) {
    if (!layer_)
        return;
    // A serial never sent is a protocol error (invalid_surface_state); a repeated or older
    // one is refused here, as `toplevel::ack_configure` does.
    if (configures_ == 0 || serial == acked_ || static_cast<std::int32_t>(serial - acked_) < 0) {
        log_layer.warn("ack_configure({}) refused: last acked {}", serial, acked_);
        return;
    }
    zwlr_layer_surface_v1_ack_configure(layer_, serial);
    acked_ = serial;
}

void layer_surface::on_configure(std::uint32_t serial, std::uint32_t width, std::uint32_t height) {
    current_.serial = serial;
    current_.size = {static_cast<std::int32_t>(std::min<std::uint32_t>(width, 1u << 30)),
                     static_cast<std::int32_t>(std::min<std::uint32_t>(height, 1u << 30))};
    current_.initial = configures_ == 0;
    ++configures_;
    if (handler_)
        handler_->on_configure(*this, current_);
}

void layer_surface::on_closed() {
    closed_ = true;
    if (handler_)
        handler_->on_closed(*this);
}

#else // !LX_WLC_LAYER

layer_surface::~layer_surface() = default;
lx::result<void> layer_surface::create(display&, surface&, const layer_options&) {
    (void)log_layer;
    return lx::not_implemented("lx::wayland::layer_surface::create");
}
void layer_surface::destroy() {}
void layer_surface::set_size(size2i) {}
void layer_surface::set_anchor(std::uint32_t) {}
void layer_surface::set_exclusive_zone(std::int32_t) {}
bool layer_surface::set_exclusive_edge(std::uint32_t) { return false; }
void layer_surface::set_margin(std::int32_t, std::int32_t, std::int32_t, std::int32_t) {}
bool layer_surface::set_keyboard_interactivity(keyboard_interactivity) { return false; }
bool layer_surface::set_layer(layer) { return false; }
void layer_surface::ack_configure(std::uint32_t) {}
void layer_surface::on_configure(std::uint32_t, std::uint32_t, std::uint32_t) {}
void layer_surface::on_closed() {}

#endif // LX_WLC_LAYER

} // namespace lx::wayland
