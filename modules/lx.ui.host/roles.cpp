module;

// Bodies of lx.ui.host:roles.

module lx.ui.host;

import std;

import lx.foundation;
import lx.runtime;
import lx.wayland.client;

namespace lx::ui::host {

namespace {

[[nodiscard]] lx::result<void> not_created(const char* what) {
    return lx::invalid_argument(what);
}

} // namespace

// ── toplevel ──

toplevel_role::~toplevel_role() { destroy(); }

lx::result<void> toplevel_role::create(surface& s, const lx::wayland::toplevel_options& options) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!s.valid())
        return not_created("lx::ui::host::toplevel_role::create: the surface is not created");
    if (valid())
        return lx::invalid_argument("lx::ui::host::toplevel_role::create: already created");
    if (auto made = toplevel_.create(s.owner().display(), s.wl(), options); !made)
        return made;
    toplevel_.set_handler(this);
    surface_ = &s;
    s.set_role(this);
    return {};
}

void toplevel_role::destroy() {
    if (surface_)
        surface_->set_role(nullptr);
    surface_ = nullptr;
    toplevel_.destroy();
}

void toplevel_role::role_ack(std::uint32_t serial) { toplevel_.ack_configure(serial); }

void toplevel_role::on_configure(lx::wayland::toplevel& t, const lx::wayland::toplevel_configure& c) {
    (void)t;
    if (!surface_)
        return;
    // A maximized or tiled size is the size; a fullscreen or resizing one the most it may
    // take (xdg-shell): the client's minimum does not apply to either.
    namespace wl = lx::wayland;
    const std::uint32_t bounded_states = wl::state_maximized | wl::state_fullscreen |
                                         wl::state_resizing | wl::state_tiled_left |
                                         wl::state_tiled_right | wl::state_tiled_top |
                                         wl::state_tiled_bottom;
    surface_->configure({c.serial, c.size, (c.states & bounded_states) != 0});
}

void toplevel_role::on_close(lx::wayland::toplevel& t) {
    (void)t;
    if (close_fn_)
        close_fn_(close_user_);
}

// ── layer surface ──

layer_role::~layer_role() { destroy(); }

lx::result<void> layer_role::create(surface& s, const layer_role_options& options) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!s.valid())
        return not_created("lx::ui::host::layer_role::create: the surface is not created");
    if (valid())
        return lx::invalid_argument("lx::ui::host::layer_role::create: already created");
    if (auto made = layer_.create(s.owner().display(), s.wl(), options.layer); !made)
        return made;
    layer_.set_handler(this);
    // The initial state goes with the initial commit (`surface::show`).
    layer_.set_size(options.size);
    if (options.anchor)
        layer_.set_anchor(options.anchor);
    if (options.exclusive_zone)
        layer_.set_exclusive_zone(options.exclusive_zone);
    if (options.margin[0] || options.margin[1] || options.margin[2] || options.margin[3])
        layer_.set_margin(options.margin[0], options.margin[1], options.margin[2], options.margin[3]);
    if (options.keyboard != lx::wayland::keyboard_interactivity::none)
        (void)layer_.set_keyboard_interactivity(options.keyboard);
    surface_ = &s;
    s.set_role(this);
    return {};
}

void layer_role::destroy() {
    if (surface_)
        surface_->set_role(nullptr);
    surface_ = nullptr;
    layer_.destroy();
}

bool layer_role::set_keyboard_interactivity(lx::wayland::keyboard_interactivity k) {
    if (!layer_.set_keyboard_interactivity(k))
        return false;
    if (surface_)
        surface_->commit_state();
    return true;
}

bool layer_role::set_layer(lx::wayland::layer l) {
    if (!layer_.set_layer(l))
        return false;
    if (surface_)
        surface_->commit_state();
    return true;
}

void layer_role::set_exclusive_zone(std::int32_t zone) {
    layer_.set_exclusive_zone(zone);
    if (surface_)
        surface_->commit_state();
}

void layer_role::role_ack(std::uint32_t serial) { layer_.ack_configure(serial); }

void layer_role::on_configure(lx::wayland::layer_surface& l, const lx::wayland::layer_configure& c) {
    (void)l;
    // 0 on an axis: the size the client asked for (the surface's options) — the role's
    // `size` is what it asked, the surface's own the fallback.
    if (surface_)
        surface_->configure({c.serial, c.size, false});
}

void layer_role::on_closed(lx::wayland::layer_surface& l) {
    (void)l;
    if (closed_fn_)
        closed_fn_(closed_user_);
}

// ── popup ──

popup_role::~popup_role() { destroy(); }

lx::result<void> popup_role::created(surface& s, lx::result<void> made) {
    if (!made)
        return made;
    popup_.set_handler(this);
    surface_ = &s;
    s.set_role(this);
    return {};
}

lx::result<void> popup_role::create(surface& s, toplevel_role& parent,
                                    const lx::wayland::positioner_desc& where) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!s.valid() || !parent.valid())
        return not_created("lx::ui::host::popup_role::create: the surface or parent is not created");
    return created(s, popup_.create(s.owner().display(), s.wl(), parent.toplevel(), where));
}

lx::result<void> popup_role::create(surface& s, popup_role& parent,
                                    const lx::wayland::positioner_desc& where) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!s.valid() || !parent.valid())
        return not_created("lx::ui::host::popup_role::create: the surface or parent is not created");
    return created(s, popup_.create(s.owner().display(), s.wl(), parent.popup(), where));
}

lx::result<void> popup_role::create(surface& s, layer_role& parent,
                                    const lx::wayland::positioner_desc& where) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!s.valid() || !parent.valid())
        return not_created("lx::ui::host::popup_role::create: the surface or parent is not created");
    return created(s, popup_.create(s.owner().display(), s.wl(), parent.layer(), where));
}

void popup_role::destroy() {
    if (surface_)
        surface_->set_role(nullptr);
    surface_ = nullptr;
    popup_.destroy();
}

void popup_role::grab(std::uint32_t serial) {
    if (!surface_)
        return;
    if (lx::wayland::seat* seat = surface_->owner().display().default_seat())
        popup_.grab(*seat, serial);
}

void popup_role::reposition(const lx::wayland::positioner_desc& where, std::uint32_t token) {
    popup_.reposition(where, token);
}

void popup_role::role_ack(std::uint32_t serial) { popup_.ack_configure(serial); }

void popup_role::on_configure(lx::wayland::popup& p, const lx::wayland::popup_configure& c) {
    (void)p;
    // The geometry's size is the popup's (the compositor's constraint solution).
    if (surface_)
        surface_->configure({c.serial, {c.geometry.width, c.geometry.height}, true});
}

void popup_role::on_done(lx::wayland::popup& p) {
    (void)p;
    if (done_fn_)
        done_fn_(done_user_);
}

// ── subsurface ──

subsurface_role::~subsurface_role() { destroy(); }

lx::result<void> subsurface_role::create(surface& s, lx::wayland::surface& parent, lx::point2i at) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!s.valid())
        return not_created("lx::ui::host::subsurface_role::create: the surface is not created");
    if (valid())
        return lx::invalid_argument("lx::ui::host::subsurface_role::create: already created");
    if (auto made = sub_.create(s.owner().display(), s.wl(), parent); !made)
        return made;
    sub_.set_position(at);
    // Its own frame callbacks: a desynchronized child commits without waiting for the parent.
    sub_.set_desync();
    surface_ = &s;
    s.set_role(this);
    // No configure event: the client's size is the size, drawn at once.
    s.configure({0, {}, false});
    return {};
}

void subsurface_role::destroy() {
    if (surface_)
        surface_->set_role(nullptr);
    surface_ = nullptr;
    sub_.destroy();
}

void subsurface_role::resize(lx::size2i size) {
    if (surface_)
        surface_->configure({0, size, true});
}

} // namespace lx::ui::host
