// lx.wayland.client built without wlr-layer-shell's glue (AGENTS.md §1.1: the build succeeds
// with any subset present). CMake compiles display.cpp, layer.cpp and xdg.cpp into this test
// a second time with LUMEN_PROTOCOL_WLR_LAYER_SHELL_UNSTABLE_V1 undefined; the linker takes
// those objects before the library's, so these are the copies that run. The layer gate is
// its own (LX_WLC_LAYER): the rest of the wrapper stays real. Against an in-process
// compositor (which does offer zwlr_layer_shell_v1):
//   - the layer surface and a popup on one return not_implemented;
//   - the global is not bound (`has(layer_shell)` false, a version of 0) and reaches
//     `global_handler::on_global` like any extension, so a caller can bind it itself;
//   - a toplevel still works.
#include <poll.h>
#include <stdio.h>

#include "../modules/lx.wayland.client/client_glue.h"

import std;
import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.compositor;
import lx.wayland.client;

#ifdef LUMEN_PROTOCOL_WLR_LAYER_SHELL_UNSTABLE_V1
#error "this unit is compiled with the layer-shell protocol macro undefined"
#endif
static_assert(LX_WLC_LAYER == 0, "no layer-shell glue: the layer gate is closed");

#if !LX_WLC_REAL

int main() {
    std::printf("SKIP: needs libwayland-client and the protocol glue\n");
    return 0;
}

#else

static_assert(LX_WLC_REAL == 1, "the layer gate does not close the rest of the wrapper");

namespace {

const char* const k_socket = "lumen-test-wayland-client-nolayer";
int g_failed = 0;

void check(const char* name, bool ok, const std::string& detail) {
    std::printf("  %s  %s — %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

struct extension_log final : lx::wayland::global_handler {
    unsigned layer_shell = 0;
    std::uint32_t version = 0;
    void on_global(lx::wayland::display&, std::uint32_t, const char* interface,
                   std::uint32_t v) override {
        if (std::strcmp(interface, "zwlr_layer_shell_v1") == 0) {
            ++layer_shell;
            version = v;
        }
    }
};

struct configure_log final : lx::wayland::toplevel_handler {
    unsigned configures = 0;
    void on_configure(lx::wayland::toplevel&, const lx::wayland::toplevel_configure&) override {
        ++configures;
    }
};

bool is_not_implemented(const lx::result<void>& r) {
    return !r && r.get_error().domain == lx::error_domain::not_implemented;
}

} // namespace

int main() {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.present = lx::compositor::present_backend::cpu;
    cfg.virtual_outputs = "640x480";
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    std::atomic<bool> pump_on{true};
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int loop_fd = comp.wayland().event_fd();
        while (pump_on.load(std::memory_order_acquire)) {
            (void)comp.wayland().dispatch(0);
            comp.wayland().flush();
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 5);
        }
    });

    {
        lx::wayland::display d;
        extension_log ext{};
        d.set_global_handler(&ext);
        const auto connected = d.connect({.name = k_socket});
        check("connect: the wrapper is real without the layer-shell glue", connected.ok(),
              connected ? "connected" : lx::format_error(connected.get_error()));
        if (connected) {
            using gk = lx::wayland::global_kind;
            check("globals: zwlr_layer_shell_v1 not bound, handed to global_handler::on_global",
                  !d.has(gk::layer_shell) && d.native_global(gk::layer_shell) == nullptr &&
                      ext.layer_shell == 1 && ext.version >= 1,
                  std::format("bound v{}, on_global saw it {} time(s) at v{}",
                              d.version(gk::layer_shell), ext.layer_shell, ext.version));

            lx::wayland::surface s;
            lx::wayland::layer_surface panel;
            const bool surface_ok = s.create(d).ok();
            const auto made = panel.create(d, s, {.name_space = "panel"});
            check("layer_surface::create: not_implemented", surface_ok && is_not_implemented(made),
                  made ? "created?" : lx::format_error(made.get_error()));
            check("layer_surface setters: nothing sent",
                  !panel.valid() && !panel.set_layer(lx::wayland::layer::overlay) &&
                      !panel.set_keyboard_interactivity(lx::wayland::keyboard_interactivity::on_demand) &&
                      !panel.set_exclusive_edge(lx::wayland::anchor_top),
                  "set_layer / set_keyboard_interactivity / set_exclusive_edge return false");

            lx::wayland::surface ps;
            lx::wayland::popup menu;
            lx::wayland::positioner_desc where{};
            where.size = {100, 50};
            where.anchor_rect = {0, 0, 10, 10};
            const auto popped = ps.create(d).ok() ? menu.create(d, ps, panel, where)
                                                  : lx::result<void>{lx::invalid_argument("surface")};
            check("popup::create on a layer parent: not_implemented",
                  is_not_implemented(popped) && !menu.valid(),
                  popped ? "created?" : lx::format_error(popped.get_error()));

            lx::wayland::toplevel top;
            configure_log log{};
            const bool top_ok = top.create(d, s, {.app_id = "lumen.test.nolayer"}).ok();
            top.set_handler(&log);
            s.commit();
            for (int i = 0; i < 100 && log.configures == 0; ++i)
                (void)d.dispatch(10);
            check("toplevel: still created and configured", top_ok && log.configures >= 1 && !d.failed(),
                  std::format("{} configure(s)", log.configures));
            top.destroy();
            ps.destroy();
            s.destroy();
        }
        d.set_global_handler(nullptr);
        d.disconnect();
    }

    pump_on.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d failed\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
