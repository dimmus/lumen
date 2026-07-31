// Verify advertised Wayland globals are real: present in the registry and bindable.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#endif
#endif

import lx.foundation;
import lx.compositor;

#if !defined(LUMEN_HAS_WAYLAND)

int main() {
    std::printf("SKIP: LUMEN_HAS_WAYLAND not set\n");
    return 0;
}

#else

namespace {

struct registry_global {
    uint32_t name = 0;
    std::string interface;
    uint32_t version = 0;
};

struct client_state {
    std::vector<registry_global> globals;
};

void registry_global(void* data, wl_registry*, uint32_t name, const char* iface, uint32_t version) {
    auto* st = static_cast<client_state*>(data);
    st->globals.push_back({name, iface ? iface : "", version});
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

const wl_interface* lookup_interface(const std::string& name) {
    if (name == "wl_compositor")
        return &wl_compositor_interface;
    if (name == "wl_subcompositor")
        return &wl_subcompositor_interface;
    if (name == "wl_seat")
        return &wl_seat_interface;
    if (name == "wl_output")
        return &wl_output_interface;
    if (name == "wl_data_device_manager")
        return &wl_data_device_manager_interface;
    if (name == "wl_shm")
        return &wl_shm_interface;
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (name == "xdg_wm_base")
        return &xdg_wm_base_interface;
    if (name == "zwp_linux_dmabuf_v1")
        return &zwp_linux_dmabuf_v1_interface;
#endif
    return nullptr;
}

} // namespace

int main() {
    const char* socket = "lumen-test-bind";
    lx::compositor::config cfg{};
    cfg.socket_name = socket;
    cfg.target_fps = 30.0;

    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }

    // Pump the Wayland event loop directly — the runtime event_loop does not yet
    // poll the display fd, so clients would otherwise hang waiting for registry replies.
    std::atomic<bool> pump{true};
    std::thread pump_thread([&] {
        while (pump.load(std::memory_order_acquire)) {
            (void)comp.wayland().dispatch(20);
            comp.wayland().flush();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    wl_display* display = wl_display_connect(socket);
    if (!display) {
        std::fprintf(stderr, "wl_display_connect(%s) failed\n", socket);
        pump.store(false);
        pump_thread.join();
        return 1;
    }

    client_state st{};
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &st);
    if (wl_display_roundtrip(display) < 0) {
        std::fprintf(stderr, "FAIL: registry roundtrip\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        pump.store(false);
        pump_thread.join();
        return 1;
    }

    if (st.globals.empty()) {
        std::fprintf(stderr, "FAIL: registry empty\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        pump.store(false);
        pump_thread.join();
        return 1;
    }

    unsigned bound = 0;
    unsigned failed = 0;
    unsigned skipped = 0;
    for (const auto& g : st.globals) {
        const wl_interface* iface = lookup_interface(g.interface);
        if (!iface) {
            ++skipped;
            std::printf("SKIP bind %s (no client iface in test)\n", g.interface.c_str());
            continue;
        }
        const uint32_t ver = g.version < static_cast<uint32_t>(iface->version)
                                 ? g.version
                                 : static_cast<uint32_t>(iface->version);
        void* obj = wl_registry_bind(registry, g.name, iface, ver > 0 ? ver : 1);
        if (!obj) {
            ++failed;
            std::fprintf(stderr, "FAIL bind %s\n", g.interface.c_str());
            continue;
        }
        // Flush bind request so the compositor creates the resource.
        wl_display_roundtrip(display);
        ++bound;
        wl_proxy_destroy(static_cast<wl_proxy*>(obj));
    }

    std::printf("protocol_bind: %zu globals, %u bound, %u failed, %u skipped\n",
                st.globals.size(), bound, failed, skipped);
    for (const auto& g : st.globals)
        std::printf("  %s v%u\n", g.interface.c_str(), g.version);

    bool has_compositor = false;
    bool has_seat = false;
    bool has_shm = false;
    for (const auto& g : st.globals) {
        if (g.interface == "wl_compositor")
            has_compositor = true;
        if (g.interface == "wl_seat")
            has_seat = true;
        if (g.interface == "wl_shm")
            has_shm = true;
    }

    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    pump.store(false, std::memory_order_release);
    pump_thread.join();
    // Destructor stops render/worker threads started by start().

    if (!has_compositor || !has_seat || !has_shm || failed > 0)
        return 1;
    return 0;
}

#endif
