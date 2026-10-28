module;

// Bodies of lx.wayland.client:xdg — xdg_surface / xdg_toplevel / xdg_popup, server-side
// decoration negotiation. A configure sequence is latched into `pending_` by the role's
// events and becomes `current_` at xdg_surface.configure, when the handler sees it whole.

#include "client_glue.h"

module lx.wayland.client;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;

namespace lx::wayland {

namespace {
constexpr lx::trace::category log_xdg{"wayland.client.xdg"};
} // namespace

#if LX_WLC_REAL

namespace {

void tag(void* proxy) { wl_proxy_set_tag(static_cast<wl_proxy*>(proxy), &lx_wlc_tag); }

const xdg_surface_listener k_toplevel_surface_listener = {
    .configure = [](void* data, xdg_surface*,
                    std::uint32_t serial) { static_cast<toplevel*>(data)->on_surface_configure(serial); },
};

const xdg_surface_listener k_popup_surface_listener = {
    .configure = [](void* data, xdg_surface*,
                    std::uint32_t serial) { static_cast<popup*>(data)->on_surface_configure(serial); },
};

const xdg_toplevel_listener k_toplevel_listener = {
    .configure =
        [](void* data, xdg_toplevel*, std::int32_t w, std::int32_t h, wl_array* states) {
            static_cast<toplevel*>(data)->on_toplevel_configure(
                w, h, static_cast<const std::uint32_t*>(states->data),
                states->size / sizeof(std::uint32_t));
        },
    .close = [](void* data, xdg_toplevel*) { static_cast<toplevel*>(data)->on_close(); },
    .configure_bounds = [](void* data, xdg_toplevel*, std::int32_t w,
                           std::int32_t h) { static_cast<toplevel*>(data)->on_bounds(w, h); },
    .wm_capabilities =
        [](void* data, xdg_toplevel*, wl_array* caps) {
            static_cast<toplevel*>(data)->on_wm_capabilities(
                static_cast<const std::uint32_t*>(caps->data), caps->size / sizeof(std::uint32_t));
        },
};

const zxdg_toplevel_decoration_v1_listener k_decoration_listener = {
    .configure = [](void* data, zxdg_toplevel_decoration_v1*,
                    std::uint32_t mode) { static_cast<toplevel*>(data)->on_decoration(mode); },
};

const xdg_popup_listener k_popup_listener = {
    .configure =
        [](void* data, xdg_popup*, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) {
            static_cast<popup*>(data)->on_popup_configure(x, y, w, h);
        },
    .popup_done = [](void* data, xdg_popup*) { static_cast<popup*>(data)->on_done(); },
    .repositioned = [](void* data, xdg_popup*,
                       std::uint32_t token) { static_cast<popup*>(data)->on_repositioned(token); },
};

xdg_positioner* make_positioner(xdg_wm_base* wm, std::uint32_t wm_version,
                                const positioner_desc& p) {
    xdg_positioner* pos = xdg_wm_base_create_positioner(wm);
    if (!pos)
        return nullptr;
    xdg_positioner_set_size(pos, std::max(1, p.size.width), std::max(1, p.size.height));
    xdg_positioner_set_anchor_rect(pos, p.anchor_rect.x, p.anchor_rect.y,
                                   std::max(1, p.anchor_rect.width),
                                   std::max(1, p.anchor_rect.height));
    xdg_positioner_set_anchor(pos, p.anchor);
    xdg_positioner_set_gravity(pos, p.gravity);
    xdg_positioner_set_constraint_adjustment(pos, p.constraint_adjustment);
    if (p.offset.x || p.offset.y)
        xdg_positioner_set_offset(pos, p.offset.x, p.offset.y);
    if (wm_version >= XDG_POSITIONER_SET_REACTIVE_SINCE_VERSION) {
        if (p.reactive)
            xdg_positioner_set_reactive(pos);
        if (!p.parent_size.empty())
            xdg_positioner_set_parent_size(pos, p.parent_size.width, p.parent_size.height);
        if (p.parent_configure)
            xdg_positioner_set_parent_configure(pos, p.parent_configure);
    }
    return pos;
}

} // namespace

// ── toplevel ──

toplevel::~toplevel() { destroy(); }

lx::result<void> toplevel::create(display& d, surface& s, const toplevel_options& options) {
    d.check_affinity();
    if (toplevel_)
        return lx::invalid_argument("lx::wayland::toplevel::create: already created");
    auto* wm = static_cast<xdg_wm_base*>(d.native_global(global_kind::xdg_wm_base));
    if (!wm || !s.valid())
        return lx::invalid_argument("lx::wayland::toplevel::create: no xdg_wm_base or surface");
    display_ = &d;
    surface_ = &s;
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm, static_cast<wl_surface*>(s.native()));
    if (!xdg_surface_)
        return lx::make_error(lx::wayland_err::object_failed, "get_xdg_surface failed");
    tag(xdg_surface_);
    xdg_surface_add_listener(xdg_surface_, &k_toplevel_surface_listener, this);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    if (!toplevel_) {
        destroy();
        return lx::make_error(lx::wayland_err::object_failed, "get_toplevel failed");
    }
    tag(toplevel_);
    xdg_toplevel_add_listener(toplevel_, &k_toplevel_listener, this);
    pending_ = {};
    current_ = {};
    acked_ = 0;
    configures_ = 0;
    if (options.title)
        xdg_toplevel_set_title(toplevel_, options.title);
    if (options.app_id)
        xdg_toplevel_set_app_id(toplevel_, options.app_id);
    if (auto* m = static_cast<zxdg_decoration_manager_v1*>(d.native_global(global_kind::decoration))) {
        decoration_ = zxdg_decoration_manager_v1_get_toplevel_decoration(m, toplevel_);
        if (decoration_) {
            tag(decoration_);
            zxdg_toplevel_decoration_v1_add_listener(decoration_, &k_decoration_listener, this);
            zxdg_toplevel_decoration_v1_set_mode(
                decoration_, options.server_side_decorations
                                 ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                 : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
            // Until the compositor says otherwise: what was asked for.
            pending_.decoration = options.server_side_decorations ? decoration_mode::server_side
                                                                  : decoration_mode::client_side;
        }
    }
    return {};
}

void toplevel::destroy() {
    // Children before parents: decoration, role, xdg_surface (protocol order).
    if (decoration_)
        zxdg_toplevel_decoration_v1_destroy(decoration_);
    decoration_ = nullptr;
    if (toplevel_)
        xdg_toplevel_destroy(toplevel_);
    toplevel_ = nullptr;
    if (xdg_surface_)
        xdg_surface_destroy(xdg_surface_);
    xdg_surface_ = nullptr;
    surface_ = nullptr;
}

void toplevel::set_title(const char* title) {
    if (toplevel_ && title)
        xdg_toplevel_set_title(toplevel_, title);
}

void toplevel::set_app_id(const char* app_id) {
    if (toplevel_ && app_id)
        xdg_toplevel_set_app_id(toplevel_, app_id);
}

void toplevel::set_min_size(size2i size) {
    if (toplevel_)
        xdg_toplevel_set_min_size(toplevel_, std::max(0, size.width), std::max(0, size.height));
}

void toplevel::set_max_size(size2i size) {
    if (toplevel_)
        xdg_toplevel_set_max_size(toplevel_, std::max(0, size.width), std::max(0, size.height));
}

void toplevel::set_maximized(bool on) {
    if (!toplevel_)
        return;
    if (on)
        xdg_toplevel_set_maximized(toplevel_);
    else
        xdg_toplevel_unset_maximized(toplevel_);
}

void toplevel::set_fullscreen(bool on, unsigned output_slot) {
    if (!toplevel_)
        return;
    if (!on) {
        xdg_toplevel_unset_fullscreen(toplevel_);
        return;
    }
    const output* o = display_ ? display_->output_at(output_slot) : nullptr;
    xdg_toplevel_set_fullscreen(toplevel_, o ? static_cast<wl_output*>(o->native()) : nullptr);
}

void toplevel::set_minimized() {
    if (toplevel_)
        xdg_toplevel_set_minimized(toplevel_);
}

void toplevel::set_parent(toplevel* parent) {
    if (toplevel_)
        xdg_toplevel_set_parent(toplevel_, parent ? parent->toplevel_ : nullptr);
}

void toplevel::set_window_geometry(rect2i r) {
    if (xdg_surface_ && !r.size().empty())
        xdg_surface_set_window_geometry(xdg_surface_, r.x, r.y, r.width, r.height);
}

void toplevel::move(seat& s, std::uint32_t serial) {
    if (toplevel_ && s.bound())
        xdg_toplevel_move(toplevel_, static_cast<wl_seat*>(s.native()), serial);
}

void toplevel::resize(seat& s, std::uint32_t serial, resize_edge edges) {
    if (toplevel_ && s.bound())
        xdg_toplevel_resize(toplevel_, static_cast<wl_seat*>(s.native()), serial,
                            static_cast<std::uint32_t>(edges));
}

void toplevel::show_window_menu(seat& s, std::uint32_t serial, point2i at) {
    if (toplevel_ && s.bound())
        xdg_toplevel_show_window_menu(toplevel_, static_cast<wl_seat*>(s.native()), serial, at.x,
                                      at.y);
}

void toplevel::request_decoration(decoration_mode mode) {
    if (!decoration_)
        return;
    if (mode == decoration_mode::none)
        zxdg_toplevel_decoration_v1_unset_mode(decoration_);
    else
        zxdg_toplevel_decoration_v1_set_mode(decoration_,
                                             mode == decoration_mode::server_side
                                                 ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                 : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

void toplevel::ack_configure(std::uint32_t serial) {
    if (!xdg_surface_ || serial == 0)
        return;
    // Serials only go forward; acking one twice or an older one is invalid_serial.
    if (configures_ == 0 || serial == acked_ ||
        static_cast<std::int32_t>(serial - acked_) < 0) {
        log_xdg.warn("ack_configure({}) refused: last acked {}", serial, acked_);
        return;
    }
    xdg_surface_ack_configure(xdg_surface_, serial);
    acked_ = serial;
}

void toplevel::on_toplevel_configure(std::int32_t w, std::int32_t h, const std::uint32_t* states,
                                     std::size_t count) {
    pending_.size = {std::max(0, w), std::max(0, h)};
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (states[i] >= 1 && states[i] <= 32)
            bits |= 1u << (states[i] - 1);
    pending_.states = bits;
}

void toplevel::on_bounds(std::int32_t w, std::int32_t h) {
    pending_.bounds = {std::max(0, w), std::max(0, h)};
}

void toplevel::on_wm_capabilities(const std::uint32_t* caps, std::size_t count) {
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (caps[i] >= 1 && caps[i] <= 32)
            bits |= 1u << (caps[i] - 1);
    pending_.wm_capabilities = bits;
}

void toplevel::on_decoration(std::uint32_t mode) {
    pending_.decoration = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                              ? decoration_mode::server_side
                              : decoration_mode::client_side;
}

void toplevel::on_surface_configure(std::uint32_t serial) {
    pending_.serial = serial;
    pending_.initial = configures_ == 0;
    ++configures_;
    // Everything stays latched: a sequence need not resend xdg_toplevel.configure (a
    // decoration-only change ends in an xdg_surface.configure alone), so the size and
    // states of the last one still hold.
    current_ = pending_;
    if (handler_)
        handler_->on_configure(*this, current_);
}

void toplevel::on_close() {
    if (handler_)
        handler_->on_close(*this);
}

// ── popup ──

popup::~popup() { destroy(); }

lx::result<void> popup::create(display& d, surface& s, toplevel& parent,
                               const positioner_desc& where) {
    return create_on(d, s, parent.native_xdg_surface(), where);
}

lx::result<void> popup::create(display& d, surface& s, popup& parent,
                               const positioner_desc& where) {
    return create_on(d, s, parent.native_xdg_surface(), where);
}

lx::result<void> popup::create_on(display& d, surface& s, void* parent_xdg,
                                  const positioner_desc& where) {
    d.check_affinity();
    if (popup_)
        return lx::invalid_argument("lx::wayland::popup::create: already created");
    auto* wm = static_cast<xdg_wm_base*>(d.native_global(global_kind::xdg_wm_base));
    if (!wm || !s.valid() || !parent_xdg)
        return lx::invalid_argument("lx::wayland::popup::create: no xdg_wm_base, surface or parent");
    display_ = &d;
    surface_ = &s;
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm, static_cast<wl_surface*>(s.native()));
    if (!xdg_surface_)
        return lx::make_error(lx::wayland_err::object_failed, "get_xdg_surface failed");
    tag(xdg_surface_);
    xdg_surface_add_listener(xdg_surface_, &k_popup_surface_listener, this);
    xdg_positioner* pos = make_positioner(wm, d.version(global_kind::xdg_wm_base), where);
    if (!pos) {
        destroy();
        return lx::make_error(lx::wayland_err::object_failed, "create_positioner failed");
    }
    popup_ = xdg_surface_get_popup(xdg_surface_, static_cast<xdg_surface*>(parent_xdg), pos);
    xdg_positioner_destroy(pos);
    if (!popup_) {
        destroy();
        return lx::make_error(lx::wayland_err::object_failed, "get_popup failed");
    }
    tag(popup_);
    xdg_popup_add_listener(popup_, &k_popup_listener, this);
    pending_ = {};
    current_ = {};
    configures_ = 0;
    done_ = false;
    return {};
}

void popup::destroy() {
    if (popup_)
        xdg_popup_destroy(popup_);
    popup_ = nullptr;
    if (xdg_surface_)
        xdg_surface_destroy(xdg_surface_);
    xdg_surface_ = nullptr;
    surface_ = nullptr;
}

void popup::grab(seat& s, std::uint32_t serial) {
    if (popup_ && s.bound())
        xdg_popup_grab(popup_, static_cast<wl_seat*>(s.native()), serial);
}

void popup::reposition(const positioner_desc& where, std::uint32_t token) {
    if (!popup_ || !display_ ||
        display_->version(global_kind::xdg_wm_base) < XDG_POPUP_REPOSITION_SINCE_VERSION)
        return;
    auto* wm = static_cast<xdg_wm_base*>(display_->native_global(global_kind::xdg_wm_base));
    xdg_positioner* pos = make_positioner(wm, display_->version(global_kind::xdg_wm_base), where);
    if (!pos)
        return;
    xdg_popup_reposition(popup_, pos, token);
    xdg_positioner_destroy(pos);
}

void popup::ack_configure(std::uint32_t serial) {
    if (xdg_surface_ && serial)
        xdg_surface_ack_configure(xdg_surface_, serial);
}

void popup::on_popup_configure(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) {
    pending_.geometry = {x, y, w, h};
}

void popup::on_repositioned(std::uint32_t token) { pending_.reposition_token = token; }

void popup::on_surface_configure(std::uint32_t serial) {
    pending_.serial = serial;
    pending_.initial = configures_ == 0;
    ++configures_;
    current_ = pending_;
    pending_.reposition_token = 0;
    if (handler_)
        handler_->on_configure(*this, current_);
}

void popup::on_done() {
    done_ = true;
    if (handler_)
        handler_->on_done(*this);
}

#else // !LX_WLC_REAL

toplevel::~toplevel() = default;
lx::result<void> toplevel::create(display&, surface&, const toplevel_options&) {
    (void)log_xdg;
    return lx::not_implemented("lx::wayland::toplevel::create");
}
void toplevel::destroy() {}
void toplevel::set_title(const char*) {}
void toplevel::set_app_id(const char*) {}
void toplevel::set_min_size(size2i) {}
void toplevel::set_max_size(size2i) {}
void toplevel::set_maximized(bool) {}
void toplevel::set_fullscreen(bool, unsigned) {}
void toplevel::set_minimized() {}
void toplevel::set_parent(toplevel*) {}
void toplevel::set_window_geometry(rect2i) {}
void toplevel::move(seat&, std::uint32_t) {}
void toplevel::resize(seat&, std::uint32_t, resize_edge) {}
void toplevel::show_window_menu(seat&, std::uint32_t, point2i) {}
void toplevel::request_decoration(decoration_mode) {}
void toplevel::ack_configure(std::uint32_t) {}
void toplevel::on_toplevel_configure(std::int32_t, std::int32_t, const std::uint32_t*, std::size_t) {}
void toplevel::on_bounds(std::int32_t, std::int32_t) {}
void toplevel::on_wm_capabilities(const std::uint32_t*, std::size_t) {}
void toplevel::on_decoration(std::uint32_t) {}
void toplevel::on_surface_configure(std::uint32_t) {}
void toplevel::on_close() {}
popup::~popup() = default;
lx::result<void> popup::create(display&, surface&, toplevel&, const positioner_desc&) {
    return lx::not_implemented("lx::wayland::popup::create");
}
lx::result<void> popup::create(display&, surface&, popup&, const positioner_desc&) {
    return lx::not_implemented("lx::wayland::popup::create");
}
lx::result<void> popup::create_on(display&, surface&, void*, const positioner_desc&) {
    return lx::not_implemented("lx::wayland::popup::create");
}
void popup::destroy() {}
void popup::grab(seat&, std::uint32_t) {}
void popup::reposition(const positioner_desc&, std::uint32_t) {}
void popup::ack_configure(std::uint32_t) {}
void popup::on_popup_configure(std::int32_t, std::int32_t, std::int32_t, std::int32_t) {}
void popup::on_repositioned(std::uint32_t) {}
void popup::on_surface_configure(std::uint32_t) {}
void popup::on_done() {}

#endif // LX_WLC_REAL

} // namespace lx::wayland
