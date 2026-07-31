module;

import lx.foundation;
import lx.wayland.server;

export module lx.compositor.xwayland;

export namespace lx::compositor::xwayland {

/// Rootless XWayland client launcher — optional, off by default (`LUMEN_BUILD_XWAYLAND`).
class bridge {
public:
    [[nodiscard]] lx::result<void> start(wayland::server& server, const char* display = ":1");
    void stop();
    [[nodiscard]] bool running() const;

private:
    bool running_ = false;
};

} // namespace lx::compositor::xwayland


lx::result<void> lx::compositor::xwayland::bridge::start(wayland::server& server,
                                                            const char* display) {
    (void)server;
    (void)display;
    return lx::not_implemented("lx::compositor::xwayland::bridge::start");
}

void lx::compositor::xwayland::bridge::stop() { running_ = false; }

bool lx::compositor::xwayland::bridge::running() const { return running_; }
