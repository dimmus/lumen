module;

// Bodies of lx.wayland.client:display — connection, registry, outputs, the read protocol and
// the lx.runtime event source.

#include "client_glue.h"

module lx.wayland.client;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;

namespace lx::wayland {

namespace {

constexpr lx::trace::category log_wl{"wayland.client"};

struct global_desc {
    const char* interface;
    unsigned cap;
    bool required;
};

// Index = global_kind. Caps are the versions this wrapper implements (types.cppm).
constexpr global_desc k_globals[k_global_kinds] = {
    {"wl_compositor", 6, true},
    {"wl_subcompositor", 1, false},
    {"wl_shm", 2, true},
    {"xdg_wm_base", 7, true},
    {"zxdg_decoration_manager_v1", 2, false},
    {"zwp_linux_dmabuf_v1", 5, false},
    {"wp_linux_drm_syncobj_manager_v1", 1, false},
    {"wp_viewporter", 1, false},
    {"wp_fractional_scale_manager_v1", 1, false},
    {"wp_presentation", 2, false},
    {"zwp_text_input_manager_v3", 2, false},
    {"wp_cursor_shape_manager_v1", 2, false},
    {"wp_single_pixel_buffer_manager_v1", 1, false},
    {"xdg_activation_v1", 1, false},
    {"zxdg_output_manager_v1", 3, false},
};

constexpr unsigned k_seat_cap = 10;
constexpr unsigned k_output_cap = 4;

void copy_string(char* dst, std::size_t cap, const char* src) {
    if (!src)
        src = "";
    std::size_t n = std::strlen(src);
    if (n >= cap)
        n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

} // namespace

#if LX_WLC_REAL

namespace {

const wl_interface* interface_of(global_kind k) {
    switch (k) {
    case global_kind::compositor: return &wl_compositor_interface;
    case global_kind::subcompositor: return &wl_subcompositor_interface;
    case global_kind::shm: return &wl_shm_interface;
    case global_kind::xdg_wm_base: return &xdg_wm_base_interface;
    case global_kind::decoration: return &zxdg_decoration_manager_v1_interface;
    case global_kind::dmabuf: return &zwp_linux_dmabuf_v1_interface;
    case global_kind::syncobj: return &wp_linux_drm_syncobj_manager_v1_interface;
    case global_kind::viewporter: return &wp_viewporter_interface;
    case global_kind::fractional_scale: return &wp_fractional_scale_manager_v1_interface;
    case global_kind::presentation: return &wp_presentation_interface;
    case global_kind::text_input: return &zwp_text_input_manager_v3_interface;
    case global_kind::cursor_shape: return &wp_cursor_shape_manager_v1_interface;
    case global_kind::single_pixel: return &wp_single_pixel_buffer_manager_v1_interface;
    case global_kind::activation: return &xdg_activation_v1_interface;
    case global_kind::xdg_output: return &zxdg_output_manager_v1_interface;
    case global_kind::count: break;
    }
    return nullptr;
}

/// The destructor request of each singleton (release where the protocol has one).
void destroy_global(global_kind k, void* p, std::uint32_t version) {
    if (!p)
        return;
    switch (k) {
    case global_kind::compositor: wl_compositor_destroy(static_cast<wl_compositor*>(p)); break;
    case global_kind::subcompositor:
        wl_subcompositor_destroy(static_cast<wl_subcompositor*>(p));
        break;
    case global_kind::shm:
        if (version >= WL_SHM_RELEASE_SINCE_VERSION)
            wl_shm_release(static_cast<wl_shm*>(p));
        else
            wl_shm_destroy(static_cast<wl_shm*>(p));
        break;
    case global_kind::xdg_wm_base: xdg_wm_base_destroy(static_cast<xdg_wm_base*>(p)); break;
    case global_kind::decoration:
        zxdg_decoration_manager_v1_destroy(static_cast<zxdg_decoration_manager_v1*>(p));
        break;
    case global_kind::dmabuf: zwp_linux_dmabuf_v1_destroy(static_cast<zwp_linux_dmabuf_v1*>(p)); break;
    case global_kind::syncobj:
        wp_linux_drm_syncobj_manager_v1_destroy(static_cast<wp_linux_drm_syncobj_manager_v1*>(p));
        break;
    case global_kind::viewporter: wp_viewporter_destroy(static_cast<wp_viewporter*>(p)); break;
    case global_kind::fractional_scale:
        wp_fractional_scale_manager_v1_destroy(static_cast<wp_fractional_scale_manager_v1*>(p));
        break;
    case global_kind::presentation: wp_presentation_destroy(static_cast<wp_presentation*>(p)); break;
    case global_kind::text_input:
        zwp_text_input_manager_v3_destroy(static_cast<zwp_text_input_manager_v3*>(p));
        break;
    case global_kind::cursor_shape:
        wp_cursor_shape_manager_v1_destroy(static_cast<wp_cursor_shape_manager_v1*>(p));
        break;
    case global_kind::single_pixel:
        wp_single_pixel_buffer_manager_v1_destroy(
            static_cast<wp_single_pixel_buffer_manager_v1*>(p));
        break;
    case global_kind::activation: xdg_activation_v1_destroy(static_cast<xdg_activation_v1*>(p)); break;
    case global_kind::xdg_output:
        zxdg_output_manager_v1_destroy(static_cast<zxdg_output_manager_v1*>(p));
        break;
    case global_kind::count: break;
    }
}

// ── listener thunks: user data is the C++ object ──

const wl_registry_listener k_registry_listener = {
    .global =
        [](void* data, wl_registry*, std::uint32_t name, const char* iface, std::uint32_t v) {
            static_cast<display*>(data)->on_global(name, iface, v);
        },
    .global_remove =
        [](void* data, wl_registry*, std::uint32_t name) {
            static_cast<display*>(data)->on_global_remove(name);
        },
};

const wl_shm_listener k_shm_listener = {
    .format = [](void* data, wl_shm*,
                 std::uint32_t format) { static_cast<display*>(data)->on_shm_format(format); },
};

const xdg_wm_base_listener k_wm_base_listener = {
    .ping = [](void* data, xdg_wm_base*,
               std::uint32_t serial) { static_cast<display*>(data)->on_ping(serial); },
};

const wp_presentation_listener k_presentation_listener = {
    .clock_id = [](void* data, wp_presentation*,
                   std::uint32_t clk) { static_cast<display*>(data)->on_presentation_clock(clk); },
};

const wl_output_listener k_output_listener = {
    .geometry =
        [](void* data, wl_output*, std::int32_t x, std::int32_t y, std::int32_t pw,
           std::int32_t ph, std::int32_t subpixel, const char* make, const char* model,
           std::int32_t transform) {
            static_cast<output*>(data)->on_geometry(x, y, pw, ph, subpixel, make, model,
                                                    transform);
        },
    .mode =
        [](void* data, wl_output*, std::uint32_t flags, std::int32_t w, std::int32_t h,
           std::int32_t refresh) { static_cast<output*>(data)->on_mode(flags, w, h, refresh); },
    .done = [](void* data, wl_output*) { static_cast<output*>(data)->on_done(); },
    .scale = [](void* data, wl_output*,
                std::int32_t scale) { static_cast<output*>(data)->on_scale(scale); },
    .name = [](void* data, wl_output*,
               const char* name) { static_cast<output*>(data)->on_name(name); },
    .description = [](void* data, wl_output*,
                      const char* d) { static_cast<output*>(data)->on_description(d); },
};

const zxdg_output_v1_listener k_xdg_output_listener = {
    .logical_position = [](void* data, zxdg_output_v1*, std::int32_t x,
                           std::int32_t y) { static_cast<output*>(data)->on_logical_position(x, y); },
    .logical_size = [](void* data, zxdg_output_v1*, std::int32_t w,
                       std::int32_t h) { static_cast<output*>(data)->on_logical_size(w, h); },
    // Deprecated since v3: wl_output.done closes the batch.
    .done = [](void*, zxdg_output_v1*) {},
    .name = [](void*, zxdg_output_v1*, const char*) {},
    .description = [](void*, zxdg_output_v1*, const char*) {},
};

} // namespace

// ── output ──

void output::bind(display* d, wl_output* proxy, std::uint32_t global_name, std::uint32_t version) {
    display_ = d;
    proxy_ = proxy;
    version_ = version;
    info_ = {};
    info_.global_name = global_name;
    wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(proxy), &lx_wlc_tag);
    wl_output_add_listener(proxy, &k_output_listener, this);
    if (void* m = d->native_global(global_kind::xdg_output))
        bind_xdg_output(m);
}

void output::bind_xdg_output(void* manager) {
    if (xdg_ || !proxy_ || !manager)
        return;
    xdg_ = zxdg_output_manager_v1_get_xdg_output(static_cast<zxdg_output_manager_v1*>(manager),
                                                 proxy_);
    if (xdg_)
        zxdg_output_v1_add_listener(xdg_, &k_xdg_output_listener, this);
}

void output::release() {
    if (xdg_)
        zxdg_output_v1_destroy(xdg_);
    xdg_ = nullptr;
    if (proxy_) {
        if (version_ >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release(proxy_);
        else
            wl_output_destroy(proxy_);
    }
    proxy_ = nullptr;
    info_ = {};
}

void output::on_geometry(std::int32_t x, std::int32_t y, std::int32_t pw, std::int32_t ph,
                         std::int32_t subpixel, const char* make, const char* model,
                         std::int32_t transform) {
    info_.position = {x, y};
    info_.physical_mm = {pw, ph};
    info_.subpixel = subpixel;
    info_.transform = transform;
    copy_string(info_.make, sizeof(info_.make), make);
    copy_string(info_.model, sizeof(info_.model), model);
    info_.done = false;
}

void output::on_mode(std::uint32_t flags, std::int32_t w, std::int32_t h, std::int32_t refresh) {
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    info_.mode = {w, h};
    info_.refresh_mhz = refresh;
    info_.done = false;
}

void output::on_scale(std::int32_t scale) {
    info_.scale = scale > 0 ? scale : 1;
    info_.done = false;
}

void output::on_name(const char* name) { copy_string(info_.name, sizeof(info_.name), name); }

void output::on_description(const char* description) {
    copy_string(info_.description, sizeof(info_.description), description);
}

void output::on_done() { info_.done = true; }

void output::on_logical_position(std::int32_t x, std::int32_t y) {
    info_.logical.x = x;
    info_.logical.y = y;
}

void output::on_logical_size(std::int32_t w, std::int32_t h) {
    info_.logical.width = w;
    info_.logical.height = h;
}

// ── display ──

display::~display() { disconnect(); }

void display::check_affinity() const { lx::runtime::assert_affinity(affinity_); }

lx::result<void> display::connect(const connect_options& options) {
    if (display_)
        return lx::invalid_argument("lx::wayland::display::connect: already connected");
    affinity_ = options.affinity;
    check_affinity();
    track_outputs_ = options.track_outputs;
    error_ = {};
    error_logged_ = false;
    for (unsigned i = 0; i < k_global_kinds; ++i) {
        globals_[i] = {};
        globals_[i].interface = k_globals[i].interface;
        globals_[i].cap = k_globals[i].cap;
        globals_[i].required = k_globals[i].required;
        proxies_[i] = nullptr;
    }
    display_ = options.fd >= 0 ? wl_display_connect_to_fd(options.fd)
                               : wl_display_connect(options.name);
    if (!display_) {
        const lx::error err = lx::make_error(lx::wayland_err::connect_failed,
                                             "cannot connect to the Wayland display");
        log_wl.warn("connect to {} failed: {}",
                    options.name ? options.name : "$WAYLAND_DISPLAY", std::strerror(errno));
        return err;
    }
    registry_ = wl_display_get_registry(display_);
    if (!registry_)
        return fail(lx::wayland_err::object_failed, "wl_display.get_registry failed");
    wl_registry_add_listener(registry_, &k_registry_listener, this);
    // The globals, then what binding them sends at once (modes, capabilities, formats).
    for (int i = 0; i < 2; ++i)
        if (wl_display_roundtrip(display_) < 0)
            return fail(lx::wayland_err::dispatch_failed, "initial roundtrip failed");

    bool required_missing = false;
    for (unsigned i = 0; i < k_global_kinds; ++i) {
        if (globals_[i].bound || !globals_[i].required)
            continue;
        log_wl.error("required global {} is not advertised", globals_[i].interface);
        required_missing = true;
    }
    if (required_missing) {
        disconnect();
        return lx::make_error(lx::wayland_err::missing_global,
                              "a required Wayland global (wl_compositor, wl_shm or "
                              "xdg_wm_base) is missing");
    }
    // The optional globals that are absent, for the one start-up line.
    char missing[320]{};
    std::size_t used = 0;
    for (unsigned i = 0; i < k_global_kinds; ++i) {
        if (globals_[i].bound)
            continue;
        const auto r = std::format_to_n(missing + used, sizeof(missing) - 1 - used, "{}{}",
                                        used ? " " : "", globals_[i].interface);
        used = std::min(sizeof(missing) - 1, used + static_cast<std::size_t>(r.size));
    }

    if (options.default_dmabuf_feedback && version(global_kind::dmabuf) >= 4) {
        feedback_ = std::make_unique<dmabuf_feedback>();
        if (auto made = feedback_->create(*this); !made) {
            feedback_.reset();
        } else if (wl_display_roundtrip(display_) < 0) {
            return fail(lx::wayland_err::dispatch_failed, "feedback roundtrip failed");
        }
    }
    log_wl.info("connected: wl_compositor v{} xdg_wm_base v{} wl_seat×{} wl_output×{} "
                "linux-dmabuf v{} syncobj v{} fractional-scale v{} presentation v{} "
                "text-input v{}; absent: {}",
                version(global_kind::compositor), version(global_kind::xdg_wm_base),
                [this] {
                    unsigned n = 0;
                    for (const auto& s : seats_)
                        n += s.bound() ? 1u : 0u;
                    return n;
                }(),
                output_count(), version(global_kind::dmabuf), version(global_kind::syncobj),
                version(global_kind::fractional_scale), version(global_kind::presentation),
                version(global_kind::text_input), used ? missing : "none");
    return {};
}

lx::result<void> display::fail(lx::wayland_err code, const char* what) {
    note_error();
    log_wl.error("{}", what);
    disconnect();
    return lx::make_error(code, what);
}

void display::disconnect() {
    if (!display_)
        return;
    feedback_.reset();
    for (auto& s : seats_)
        s.release();
    for (auto& o : outputs_)
        o.release();
    for (unsigned i = 0; i < k_global_kinds; ++i) {
        destroy_global(static_cast<global_kind>(i), proxies_[i], globals_[i].bound);
        proxies_[i] = nullptr;
        globals_[i].bound = 0;
        globals_[i].name = 0;
    }
    if (registry_)
        wl_registry_destroy(registry_);
    registry_ = nullptr;
    wl_display_disconnect(display_);
    display_ = nullptr;
}

void display::note_error() {
    if (!display_)
        return;
    const int e = wl_display_get_error(display_);
    if (e == 0 || error_.display_errno != 0)
        return;
    error_.display_errno = e;
    if (e == EPROTO) {
        const wl_interface* iface = nullptr;
        std::uint32_t id = 0;
        error_.code = wl_display_get_protocol_error(display_, &iface, &id);
        error_.interface = iface ? iface->name : "[destroyed]";
        error_.object_id = id;
    }
    if (!error_logged_) {
        error_logged_ = true;
        if (e == EPROTO)
            log_wl.error("protocol error: {}@{} code {}", error_.interface, error_.object_id,
                         error_.code);
        else
            log_wl.error("connection lost: {}", std::strerror(e));
    }
}

int display::fd() const { return display_ ? wl_display_get_fd(display_) : -1; }

bool display::prepare_read() {
    check_affinity();
    return display_ && wl_display_prepare_read(display_) == 0;
}

lx::result<void> display::read_events() {
    if (!display_)
        return lx::make_error(lx::wayland_err::connection_lost, "not connected");
    if (wl_display_read_events(display_) < 0) {
        note_error();
        return lx::make_error(error_.display_errno == EPROTO ? lx::wayland_err::protocol_violation
                                                             : lx::wayland_err::connection_lost,
                              "wl_display_read_events failed");
    }
    return {};
}

void display::cancel_read() {
    if (display_)
        wl_display_cancel_read(display_);
}

lx::result<int> display::dispatch_pending() {
    check_affinity();
    if (!display_)
        return lx::make_error(lx::wayland_err::connection_lost, "not connected");
    const int n = wl_display_dispatch_pending(display_);
    for (auto& s : seats_)
        s.flush_pointer_frame();
    if (n < 0) {
        note_error();
        return lx::make_error(error_.display_errno == EPROTO ? lx::wayland_err::protocol_violation
                                                             : lx::wayland_err::dispatch_failed,
                              "wl_display_dispatch_pending failed");
    }
    return n;
}

lx::result<display::flush_status> display::flush() {
    check_affinity();
    if (!display_)
        return lx::make_error(lx::wayland_err::connection_lost, "not connected");
    if (wl_display_flush(display_) >= 0)
        return flush_status::done;
    if (errno == EAGAIN)
        return flush_status::again;
    note_error();
    return lx::make_error(lx::wayland_err::connection_lost, "wl_display_flush failed");
}

lx::result<int> display::dispatch(int timeout_ms) {
    check_affinity();
    if (!display_)
        return lx::make_error(lx::wayland_err::connection_lost, "not connected");
    if (wl_display_prepare_read(display_) != 0)
        return dispatch_pending();
    pollfd pfd{wl_display_get_fd(display_), POLLIN, 0};
    if (wl_display_flush(display_) < 0) {
        if (errno == EAGAIN) {
            pfd.events |= POLLOUT;
        } else if (errno != EPIPE) {
            // EPIPE: the server went away; reading still collects its last error.
            wl_display_cancel_read(display_);
            note_error();
            return lx::make_error(lx::wayland_err::connection_lost, "wl_display_flush failed");
        }
    }
    int ready = 0;
    do {
        ready = ::poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        wl_display_cancel_read(display_);
        if (ready < 0)
            return lx::make_error(lx::wayland_err::dispatch_failed, "poll on the display failed");
        return 0;
    }
    if (pfd.revents & POLLOUT)
        (void)wl_display_flush(display_);
    if (pfd.revents & (POLLIN | POLLERR | POLLHUP)) {
        if (auto r = read_events(); !r)
            return r.get_error();
    } else {
        wl_display_cancel_read(display_);
    }
    return dispatch_pending();
}

lx::result<void> display::roundtrip() {
    check_affinity();
    if (!display_)
        return lx::make_error(lx::wayland_err::connection_lost, "not connected");
    if (wl_display_roundtrip(display_) < 0) {
        note_error();
        return lx::make_error(error_.display_errno == EPROTO ? lx::wayland_err::protocol_violation
                                                             : lx::wayland_err::connection_lost,
                              "wl_display_roundtrip failed");
    }
    for (auto& s : seats_)
        s.flush_pointer_frame();
    return {};
}

void display::on_global(std::uint32_t name, const char* interface, std::uint32_t version) {
    for (unsigned i = 0; i < k_global_kinds; ++i) {
        if (std::strcmp(interface, k_globals[i].interface) != 0)
            continue;
        global_info& g = globals_[i];
        if (g.bound)
            return; // a second instance of a singleton: the first stays
        g.name = name;
        g.advertised = version;
        const std::uint32_t v = std::min<std::uint32_t>(version, g.cap);
        void* p = wl_registry_bind(registry_, name, interface_of(static_cast<global_kind>(i)), v);
        if (!p)
            return;
        g.bound = v;
        proxies_[i] = p;
        wl_proxy_set_tag(static_cast<wl_proxy*>(p), &lx_wlc_tag);
        switch (static_cast<global_kind>(i)) {
        case global_kind::shm:
            wl_shm_add_listener(static_cast<wl_shm*>(p), &k_shm_listener, this);
            break;
        case global_kind::xdg_wm_base:
            xdg_wm_base_add_listener(static_cast<xdg_wm_base*>(p), &k_wm_base_listener, this);
            break;
        case global_kind::presentation:
            wp_presentation_add_listener(static_cast<wp_presentation*>(p),
                                         &k_presentation_listener, this);
            break;
        case global_kind::xdg_output:
            for (auto& o : outputs_)
                o.bind_xdg_output(p);
            break;
        default: break;
        }
        return;
    }
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        for (auto& s : seats_) {
            if (s.bound())
                continue;
            auto* p = static_cast<wl_seat*>(wl_registry_bind(
                registry_, name, &wl_seat_interface, std::min<std::uint32_t>(version, k_seat_cap)));
            if (p)
                s.bind(this, p, name, std::min<std::uint32_t>(version, k_seat_cap));
            return;
        }
        ++dropped_globals_;
        log_wl.warn("more than {} seats: wl_seat {} ignored", k_max_seats, name);
        return;
    }
    if (std::strcmp(interface, wl_output_interface.name) == 0 && track_outputs_) {
        for (auto& o : outputs_) {
            if (o.bound())
                continue;
            const std::uint32_t v = std::min<std::uint32_t>(version, k_output_cap);
            auto* p = static_cast<wl_output*>(
                wl_registry_bind(registry_, name, &wl_output_interface, v));
            if (p)
                o.bind(this, p, name, v);
            return;
        }
        ++dropped_globals_;
        log_wl.warn("more than {} outputs: wl_output {} ignored", k_max_outputs, name);
        return;
    }
    if (global_handler_)
        global_handler_->on_global(*this, name, interface, version);
}

void display::on_global_remove(std::uint32_t name) {
    for (auto& s : seats_)
        if (s.bound() && s.global_name() == name) {
            s.release();
            return;
        }
    for (auto& o : outputs_)
        if (o.bound() && o.info().global_name == name) {
            o.release();
            return;
        }
    for (auto& g : globals_)
        if (g.bound && g.name == name) {
            // A singleton going away: its proxy stays valid until disconnect (requests on
            // it are ignored by the server); new objects are not made from it.
            log_wl.warn("global {} removed", g.interface);
            g.name = 0;
            return;
        }
    if (global_handler_)
        global_handler_->on_global_remove(*this, name);
}

void display::on_shm_format(std::uint32_t format) {
    ++shm_format_count_;
    if (format == WL_SHM_FORMAT_ARGB8888)
        shm_formats_ |= 1u;
    else if (format == WL_SHM_FORMAT_XRGB8888)
        shm_formats_ |= 2u;
}

void display::on_ping(std::uint32_t serial) {
    ++pings_;
    xdg_wm_base_pong(static_cast<xdg_wm_base*>(proxies_[static_cast<unsigned>(global_kind::xdg_wm_base)]),
                     serial);
}

void display::on_presentation_clock(std::uint32_t clock) { presentation_clock_ = clock; }

const output* display::output_at(unsigned slot) const {
    return slot < k_max_outputs && outputs_[slot].bound() ? &outputs_[slot] : nullptr;
}

unsigned display::output_slot(const void* wl_output) const {
    for (unsigned i = 0; i < k_max_outputs; ++i)
        if (outputs_[i].bound() && outputs_[i].native() == wl_output)
            return i;
    return ~0u;
}

unsigned display::output_count() const {
    unsigned n = 0;
    for (const auto& o : outputs_)
        n += o.bound() ? 1u : 0u;
    return n;
}

seat* display::default_seat() {
    for (auto& s : seats_)
        if (s.bound())
            return &s;
    return nullptr;
}

seat* display::seat_at(unsigned slot) {
    return slot < k_max_seats && seats_[slot].bound() ? &seats_[slot] : nullptr;
}

#else // !LX_WLC_REAL

void output::bind(display*, wl_output*, std::uint32_t, std::uint32_t) {}
void output::bind_xdg_output(void*) {}
void output::release() {}
void output::on_geometry(std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                         const char*, const char*, std::int32_t) {}
void output::on_mode(std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
void output::on_scale(std::int32_t) {}
void output::on_name(const char*) {}
void output::on_description(const char*) {}
void output::on_done() {}
void output::on_logical_position(std::int32_t, std::int32_t) {}
void output::on_logical_size(std::int32_t, std::int32_t) {}

display::~display() = default;
void display::check_affinity() const { lx::runtime::assert_affinity(affinity_); }
lx::result<void> display::connect(const connect_options&) {
    return lx::not_implemented("lx::wayland::display::connect");
}
lx::result<void> display::fail(lx::wayland_err code, const char* what) {
    return lx::make_error(code, what);
}
void display::disconnect() {}
void display::note_error() {}
int display::fd() const { return -1; }
bool display::prepare_read() { return false; }
lx::result<void> display::read_events() { return lx::not_implemented("lx::wayland::display::read_events"); }
void display::cancel_read() {}
lx::result<int> display::dispatch_pending() {
    return lx::not_implemented("lx::wayland::display::dispatch_pending");
}
lx::result<display::flush_status> display::flush() {
    return lx::not_implemented("lx::wayland::display::flush");
}
lx::result<int> display::dispatch(int) { return lx::not_implemented("lx::wayland::display::dispatch"); }
lx::result<void> display::roundtrip() { return lx::not_implemented("lx::wayland::display::roundtrip"); }
void display::on_global(std::uint32_t, const char*, std::uint32_t) {}
void display::on_global_remove(std::uint32_t) {}
void display::on_shm_format(std::uint32_t) {}
void display::on_ping(std::uint32_t) {}
void display::on_presentation_clock(std::uint32_t) {}
const output* display::output_at(unsigned) const { return nullptr; }
unsigned display::output_slot(const void*) const { return ~0u; }
unsigned display::output_count() const { return 0; }
seat* display::default_seat() { return nullptr; }
seat* display::seat_at(unsigned) { return nullptr; }

#endif // LX_WLC_REAL

// ── display_source (plain libwayland calls go through display, so no #if) ──

display_source::display_source(display& d, lx::runtime::event_loop& loop,
                               lx::runtime::callback on_error)
    : display_{d}, loop_{loop}, on_error_{on_error} {}

display_source::~display_source() { detach(); }

void display_source::attach() {
    if (attached_ || display_.fd() < 0)
        return;
    loop_.add_source(this);
    attached_ = true;
}

void display_source::detach() {
    if (prepared_) {
        display_.cancel_read();
        prepared_ = false;
    }
    if (attached_)
        loop_.remove_source(this);
    attached_ = false;
    want_write_ = false;
}

int display_source::poll_fd() const { return display_.fd(); }

void display_source::fail() {
    if (dead_)
        return;
    dead_ = true;
    // Off the loop: a hung-up fd would otherwise wake it forever.
    detach();
    if (on_error_)
        on_error_();
}

bool display_source::prepare(lx::runtime::clock_time& timeout) {
    timeout = -1;
    if (dead_)
        return false;
    if (!prepared_) {
        if (!display_.prepare_read()) {
            // Events are queued already: dispatch them before waiting (GTK's prepare).
            queued_ = true;
            timeout = 0;
            return true;
        }
        prepared_ = true;
        readable_ = false;
    }
    auto flushed = display_.flush();
    if (!flushed) {
        fail();
        return false;
    }
    const bool want = *flushed == display::flush_status::again;
    if (want != want_write_) {
        loop_.set_write_interest(this, want);
        want_write_ = want;
    }
    return false;
}

bool display_source::check() {
    if (dead_)
        return false;
    if (prepared_) {
        prepared_ = false;
        if (readable_) {
            readable_ = false;
            ++reads_;
            if (auto r = display_.read_events(); !r) {
                fail();
                return false;
            }
            queued_ = true;
        } else {
            display_.cancel_read();
        }
    }
    return queued_ || writable_;
}

bool display_source::dispatch() {
    if (dead_)
        return false;
    if (writable_) {
        writable_ = false;
        auto flushed = display_.flush();
        if (!flushed) {
            fail();
            return false;
        }
        const bool want = *flushed == display::flush_status::again;
        if (want != want_write_) {
            loop_.set_write_interest(this, want);
            want_write_ = want;
        }
    }
    if (queued_) {
        queued_ = false;
        if (auto n = display_.dispatch_pending(); !n) {
            fail();
            return false;
        }
    }
    return true;
}

} // namespace lx::wayland
