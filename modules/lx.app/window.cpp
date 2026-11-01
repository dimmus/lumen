module;

// Bodies of lx.app:window: open and close over an lx.ui.host surface with the toplevel role.

module lx.app;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.ui.host;
import lx.wayland.client;

namespace lx::app {

window::window() = default;

window::~window() { close(); }

void window::close_requested(void* self) {
    auto* w = static_cast<window*>(self);
    if (w->host_)
        w->host_->host_close_requested(*w);
}

lx::result<void> window::open(window_host& host, const window_options& options) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (open_)
        return lx::invalid_argument("lx::app::window::open: already open");
    if (!options.describe)
        return lx::invalid_argument("lx::app::window::open: no describe function");
    if (!host.host_display().connected())
        return lx::make_error(lx::wayland_err::connect_failed,
                              "lx::app::window::open: the application is not connected");
    if (auto admitted = host.host_attach(*this); !admitted)
        return admitted;
    host_ = &host;
    attached_ = true;

    lx::ui::host::surface_options so{};
    so.describe = options.describe;
    so.size = options.size;
    so.min_size = options.min_size;
    so.opaque = options.opaque;
    so.arena_bytes = options.arena_bytes;
    so.presentation_feedback = options.presentation_feedback;
    so.frame_records = true;
    so.frame_ring = ring_;
    if (auto made = surface_.create(host.host_context(), so); !made) {
        close();
        return made;
    }
    lx::wayland::toplevel_options to{};
    to.title = options.title;
    to.app_id = options.app_id;
    to.server_side_decorations = options.server_side_decorations;
    if (auto made = role_.create(surface_, to); !made) {
        close();
        return made;
    }
    role_.set_close_handler(&window::close_requested, this);
    if (options.min_size.width > 0 || options.min_size.height > 0)
        role_.toplevel().set_min_size(options.min_size);
    open_ = true;
    if (auto shown = surface_.show(); !shown) {
        close();
        return shown;
    }
    return {};
}

void window::close() {
    // The host forgets the window (a no-op for one it never admitted).
    if (host_ && attached_)
        host_->host_detach(*this);
    attached_ = false;
    role_.destroy();
    surface_.destroy();
    open_ = false;
    host_ = nullptr;
}

} // namespace lx::app
