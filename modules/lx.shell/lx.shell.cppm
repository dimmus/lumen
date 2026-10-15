module;

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(LUMEN_SHELL_WAYLAND)
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <wayland-client.h>
#include "cursor-shape-v1-client-protocol.h"
#include "zlm_shell_v1-client-protocol.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

import lx.foundation;
import lx.shell.policy;
import lx.trace;

export module lx.shell;

export import :task_model;
export import :bridge;
export import :panel;

export namespace lx::shell {

struct shell_config {
    /// Wayland display (socket) name; null = $WAYLAND_DISPLAY, as libwayland decides.
    const char* display = nullptr;
};

/// lumen-shell: a privileged Wayland client of lumen-compositor (architecture §2.2, §5).
///
/// One thread. It binds zlm_shell_v1 (the compositor accepts that only from its configured
/// shell binary), keeps the window list from zlm_policy_bridge_v1, and shows it as a top
/// panel on wlr-layer-shell. The loop sleeps in ppoll on the display fd and a signalfd —
/// no timers, no wakeups while nothing happens. Exit status: 0 on SIGINT/SIGTERM, 1 when
/// the compositor refuses or goes away (the systemd unit restarts it).
class shell_app {
public:
    explicit shell_app(shell_config config = {});
    ~shell_app();
    shell_app(const shell_app&) = delete;
    shell_app& operator=(const shell_app&) = delete;

    [[nodiscard]] int run();

    [[nodiscard]] policy_registry& policies();
    [[nodiscard]] policy_bridge& bridge() { return bridge_; }
    [[nodiscard]] panel& primary_panel() { return panel_; }

    /// Registry bookkeeping (defined with the listeners).
    struct events;

private:
    friend struct events;
    [[nodiscard]] int fail(const char* what);
    void teardown();

    shell_config config_{};
    policy_bridge bridge_{};
    panel panel_{};
    void* display_ = nullptr;     // wl_display*
    void* registry_ = nullptr;    // wl_registry*
    void* compositor_ = nullptr;  // wl_compositor*
    void* shm_ = nullptr;         // wl_shm*
    void* seat_ = nullptr;        // wl_seat*
    void* layer_shell_ = nullptr; // zwlr_layer_shell_v1*
    void* shell_ = nullptr;       // zlm_shell_v1*
    void* cursor_shape_ = nullptr; // wp_cursor_shape_manager_v1*
    unsigned compositor_version_ = 0;
    unsigned seat_name_ = 0;
    unsigned seat_version_ = 0;
};

} // namespace lx::shell

module :private;

namespace {

constexpr const char* k_shell_category = "shell";

void shell_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_shell_category, line);
}

} // namespace

lx::shell::shell_app::shell_app(shell_config config) : config_{config} {}
lx::shell::shell_app::~shell_app() { teardown(); }
lx::shell::policy_registry& lx::shell::shell_app::policies() {
    return policy_registry::instance();
}

#if defined(LUMEN_SHELL_WAYLAND)

namespace {

[[nodiscard]] unsigned min_version(unsigned have, unsigned want) {
    return have < want ? have : want;
}

} // namespace

struct lx::shell::shell_app::events {
    static void global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                       uint32_t version) {
        auto& a = *static_cast<shell_app*>(data);
        if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
            a.compositor_version_ = version;
            a.compositor_ = wl_registry_bind(reg, name, &wl_compositor_interface,
                                             min_version(version, 4));
        } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
            a.shm_ = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !a.seat_name_) {
            // Bound later, by the panel's creation: wl_seat.capabilities follows a bind at
            // once, and an event that reaches a proxy with no listener is dropped.
            a.seat_name_ = name;
            a.seat_version_ = version;
        } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
            a.layer_shell_ = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
                                              min_version(version, 4));
        } else if (std::strcmp(iface, zlm_shell_v1_interface.name) == 0) {
            a.shell_ = wl_registry_bind(reg, name, &zlm_shell_v1_interface, 1);
            // capabilities follows the bind at once; listen before it is dispatched.
            (void)a.bridge_.observe(a.shell_);
        } else if (std::strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
            a.cursor_shape_ = wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
        }
    }
    static void global_remove(void*, wl_registry*, uint32_t) {}
};

namespace {

const wl_registry_listener k_registry_listener = {
    .global = lx::shell::shell_app::events::global,
    .global_remove = lx::shell::shell_app::events::global_remove,
};

/// Why the connection ended, in words: the protocol error if there was one.
void describe_connection_error(wl_display* d, const char* when) {
    const int err = wl_display_get_error(d);
    char line[256];
    if (err == EPROTO) {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        const uint32_t code = wl_display_get_protocol_error(d, &iface, &id);
        std::snprintf(line, sizeof(line), "%s: protocol error on %s#%u, code %u", when,
                      iface ? iface->name : "?", id, code);
    } else {
        std::snprintf(line, sizeof(line), "%s: connection lost (%s)", when, std::strerror(err));
    }
    shell_log(lx::trace::level::error, line);
}

} // namespace

int lx::shell::shell_app::fail(const char* what) {
    shell_log(lx::trace::level::error, what);
    return 1;
}

int lx::shell::shell_app::run() {
    // Signals arrive as readable data on an fd the loop already polls: the handler cannot
    // interrupt a libwayland call halfway, and shutdown is an ordinary loop exit.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0)
        return fail("sigprocmask failed");
    const int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0)
        return fail("signalfd failed");

    auto* d = wl_display_connect(config_.display);
    if (!d) {
        close(sfd);
        char line[160];
        std::snprintf(line, sizeof(line), "cannot connect to the Wayland display %s",
                      config_.display ? config_.display : "($WAYLAND_DISPLAY)");
        return fail(line);
    }
    display_ = d;
    auto* reg = wl_display_get_registry(d);
    registry_ = reg;
    wl_registry_add_listener(reg, &k_registry_listener, this);
    // First roundtrip: the globals, bound as announced. Second: the binds themselves —
    // a compositor that does not take this process for its shell answers zlm_shell_v1
    // with a protocol error here, before anything is drawn.
    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0) {
        describe_connection_error(d, "zlm_shell_v1 bind rejected (is this the compositor's "
                                     "configured shell binary? see its 'privileged global' "
                                     "warning)");
        close(sfd);
        return 1;
    }
    if (!compositor_ || compositor_version_ < 4 || !shm_ || !layer_shell_ || !shell_) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "missing globals:%s%s%s%s", !compositor_ || compositor_version_ < 4
                          ? " wl_compositor(v4+)" : "",
                      !shm_ ? " wl_shm" : "", !layer_shell_ ? " zwlr_layer_shell_v1" : "",
                      !shell_ ? " zlm_shell_v1" : "");
        close(sfd);
        return fail(line);
    }
    if (seat_name_) {
        // v5 at most: the panel's wl_pointer listener covers every event up to v5.
        seat_ = wl_registry_bind(reg, seat_name_, &wl_seat_interface, min_version(seat_version_, 5));
    } else {
        shell_log(lx::trace::level::warn, "no wl_seat: the panel takes no input");
    }

    if (auto attached = bridge_.attach(shell_); !attached) {
        lx::trace::logger::global().log_error(attached.get_error(), k_shell_category);
        close(sfd);
        return 1;
    }
    if (auto created = panel_.create({.compositor = compositor_,
                                      .shm = shm_,
                                      .layer_shell = layer_shell_,
                                      .seat = seat_,
                                      .cursor_shape_manager = cursor_shape_},
                                     bridge_);
        !created) {
        lx::trace::logger::global().log_error(created.get_error(), k_shell_category);
        close(sfd);
        return 1;
    }
    {
        char line[160];
        std::snprintf(line, sizeof(line), "connected to %s; panel requested",
                      config_.display ? config_.display : "$WAYLAND_DISPLAY");
        shell_log(lx::trace::level::info, line);
    }

    int code = 0;
    pollfd fds[2] = {{wl_display_get_fd(d), POLLIN, 0}, {sfd, POLLIN, 0}};
    for (;;) {
        // The canonical libwayland client loop: dispatch what is queued, flush, then
        // sleep until the socket or a signal has something.
        while (wl_display_prepare_read(d) != 0) {
            if (wl_display_dispatch_pending(d) < 0) {
                describe_connection_error(d, "dispatch failed");
                code = 1;
                break;
            }
        }
        if (code)
            break;
        panel_.after_dispatch();
        if (panel_.closed()) {
            wl_display_cancel_read(d);
            shell_log(lx::trace::level::error, "panel closed; exiting so the shell is restarted");
            code = 1;
            break;
        }
        fds[0].events = POLLIN;
        if (wl_display_flush(d) < 0) {
            if (errno == EAGAIN) {
                fds[0].events |= POLLOUT;
            } else {
                wl_display_cancel_read(d);
                describe_connection_error(d, "flush failed");
                code = 1;
                break;
            }
        }
        const int n = ppoll(fds, 2, nullptr, nullptr);
        if (n < 0 && errno != EINTR) {
            wl_display_cancel_read(d);
            code = fail("ppoll failed");
            break;
        }
        if (n > 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
            if (wl_display_read_events(d) < 0) {
                describe_connection_error(d, "compositor gone");
                code = 1;
                break;
            }
        } else {
            wl_display_cancel_read(d);
        }
        if (wl_display_dispatch_pending(d) < 0) {
            describe_connection_error(d, "dispatch failed");
            code = 1;
            break;
        }
        if (n > 0 && (fds[1].revents & POLLIN)) {
            signalfd_siginfo si{};
            if (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
                char line[64];
                std::snprintf(line, sizeof(line), "signal %u: exiting", si.ssi_signo);
                shell_log(lx::trace::level::info, line);
                break;
            }
        }
    }
    close(sfd);
    teardown();
    return code;
}

void lx::shell::shell_app::teardown() {
    auto* d = static_cast<wl_display*>(display_);
    if (!d)
        return;
    panel_.destroy();
    bridge_.detach();
    if (cursor_shape_)
        wp_cursor_shape_manager_v1_destroy(static_cast<wp_cursor_shape_manager_v1*>(cursor_shape_));
    if (shell_)
        zlm_shell_v1_destroy(static_cast<zlm_shell_v1*>(shell_));
    if (layer_shell_)
        zwlr_layer_shell_v1_destroy(static_cast<zwlr_layer_shell_v1*>(layer_shell_));
    if (seat_) {
        if (wl_seat_get_version(static_cast<wl_seat*>(seat_)) >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(static_cast<wl_seat*>(seat_));
        else
            wl_seat_destroy(static_cast<wl_seat*>(seat_));
    }
    if (shm_)
        wl_shm_destroy(static_cast<wl_shm*>(shm_));
    if (compositor_)
        wl_compositor_destroy(static_cast<wl_compositor*>(compositor_));
    if (registry_)
        wl_registry_destroy(static_cast<wl_registry*>(registry_));
    (void)wl_display_flush(d);
    wl_display_disconnect(d);
    display_ = registry_ = compositor_ = shm_ = seat_ = layer_shell_ = shell_ = cursor_shape_ =
        nullptr;
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::shell_app::events {};

int lx::shell::shell_app::fail(const char* what) {
    shell_log(lx::trace::level::error, what);
    return 1;
}

int lx::shell::shell_app::run() {
    lx::trace::logger::global().log_error(lx::not_implemented("lx::shell::shell_app::run"),
                                          k_shell_category);
    return 1;
}

void lx::shell::shell_app::teardown() {}

#endif
