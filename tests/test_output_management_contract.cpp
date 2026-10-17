// wlr-output-management on the wire (ADR 008 Part C), against an in-process compositor whose
// display is virtual and whose monitors are fake connector scans — the same heads, jobs,
// switch and persistence a real display drives (the kernel's TEST_ONLY aside, which the live
// monitor-smoke covers):
//   - heads: one per output and per connected monitor, with modes, the current mode,
//     position, scale and transform, then `done`;
//   - apply: a mode, a position, a scale, a transform — wl_output, xdg-output and the head
//     events follow, with a new serial; restating the same state changes nothing;
//   - disable and enable a monitor: its wl_output goes and comes back, its head stays;
//   - a custom mode: a virtual mode object becomes the current one;
//   - test: answered without changing anything;
//   - failed: every head disabled, adaptive sync on a head that cannot do it;
//   - cancelled: a stale serial, and a configuration made before a hotplug;
//   - every protocol error, each on its own connection;
//   - a hotplugged monitor is a new head, an unplugged one is finished;
//   - persistence: the applied state is written to outputs.toml and a new compositor comes
//     up in it.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-output-unstable-v1-client-protocol.h"
#if __has_include("wlr-output-management-unstable-v1-client-protocol.h")
#include "wlr-output-management-unstable-v1-client-protocol.h"
#define LUMEN_HAS_OUTPUT_MANAGEMENT 1
#endif
#endif
#endif

#include "scan_builder.hpp"

import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.drm;
import lx.compositor;

#if !defined(LUMEN_HAS_WAYLAND) || !defined(LUMEN_HAS_PROTOCOL_GLUE) || \
    !defined(LUMEN_HAS_OUTPUT_MANAGEMENT)

int main() {
    std::printf("SKIP: needs libwayland and the generated protocol glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-outmgmt";

int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

struct mode_info {
    zwlr_output_mode_v1* mode = nullptr;
    int w = 0, h = 0, refresh = 0;
    bool preferred = false;
    bool finished = false;
};

struct head_info {
    zwlr_output_head_v1* head = nullptr;
    std::string name, description, make;
    std::vector<mode_info*> modes;
    bool enabled = false;
    mode_info* current = nullptr;
    int x = 0, y = 0;
    int transform = 0;
    double scale = 0.0;
    int adaptive = -1;
    bool finished = false;
};

struct wl_out {
    wl_output* output = nullptr;
    uint32_t global = 0;
    bool removed = false;
    std::string name;
    int w = 0, h = 0, transform = 0, scale = 1;
    zxdg_output_v1* xo = nullptr;
    int lx = 0, ly = 0, lw = 0, lh = 0;
};

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    zwlr_output_manager_v1* manager = nullptr;
    zxdg_output_manager_v1* xom = nullptr;
    std::vector<head_info*> heads;
    uint32_t serial = 0;
    unsigned dones = 0;
    std::vector<wl_out*> outs;
};

void mode_size(void* d, zwlr_output_mode_v1*, int32_t w, int32_t h) {
    auto* m = static_cast<mode_info*>(d);
    m->w = w;
    m->h = h;
}
void mode_refresh(void* d, zwlr_output_mode_v1*, int32_t r) { static_cast<mode_info*>(d)->refresh = r; }
void mode_preferred(void* d, zwlr_output_mode_v1*) { static_cast<mode_info*>(d)->preferred = true; }
void mode_finished(void* d, zwlr_output_mode_v1*) { static_cast<mode_info*>(d)->finished = true; }
const zwlr_output_mode_v1_listener mode_listener = {mode_size, mode_refresh, mode_preferred,
                                                    mode_finished};

void head_name(void* d, zwlr_output_head_v1*, const char* n) { static_cast<head_info*>(d)->name = n; }
void head_description(void* d, zwlr_output_head_v1*, const char* n) {
    static_cast<head_info*>(d)->description = n;
}
void head_physical_size(void*, zwlr_output_head_v1*, int32_t, int32_t) {}
void head_mode(void* d, zwlr_output_head_v1*, zwlr_output_mode_v1* m) {
    auto* mi = new mode_info{};
    mi->mode = m;
    zwlr_output_mode_v1_add_listener(m, &mode_listener, mi);
    static_cast<head_info*>(d)->modes.push_back(mi);
}
void head_enabled(void* d, zwlr_output_head_v1*, int32_t e) { static_cast<head_info*>(d)->enabled = e != 0; }
void head_current_mode(void* d, zwlr_output_head_v1*, zwlr_output_mode_v1* m) {
    auto* h = static_cast<head_info*>(d);
    for (auto* mi : h->modes)
        if (mi->mode == m)
            h->current = mi;
}
void head_position(void* d, zwlr_output_head_v1*, int32_t x, int32_t y) {
    auto* h = static_cast<head_info*>(d);
    h->x = x;
    h->y = y;
}
void head_transform(void* d, zwlr_output_head_v1*, int32_t t) { static_cast<head_info*>(d)->transform = t; }
void head_scale(void* d, zwlr_output_head_v1*, wl_fixed_t s) {
    static_cast<head_info*>(d)->scale = wl_fixed_to_double(s);
}
void head_finished(void* d, zwlr_output_head_v1*) { static_cast<head_info*>(d)->finished = true; }
void head_make(void* d, zwlr_output_head_v1*, const char* m) { static_cast<head_info*>(d)->make = m; }
void head_model(void*, zwlr_output_head_v1*, const char*) {}
void head_serial(void*, zwlr_output_head_v1*, const char*) {}
void head_adaptive(void* d, zwlr_output_head_v1*, uint32_t a) {
    static_cast<head_info*>(d)->adaptive = static_cast<int>(a);
}
const zwlr_output_head_v1_listener head_listener = {
    head_name,   head_description, head_physical_size, head_mode,  head_enabled,
    head_current_mode, head_position, head_transform, head_scale, head_finished,
    head_make,   head_model,       head_serial,        head_adaptive};

void mgr_head(void* d, zwlr_output_manager_v1*, zwlr_output_head_v1* h) {
    auto* hi = new head_info{};
    hi->head = h;
    zwlr_output_head_v1_add_listener(h, &head_listener, hi);
    static_cast<client*>(d)->heads.push_back(hi);
}
void mgr_done(void* d, zwlr_output_manager_v1*, uint32_t serial) {
    auto* c = static_cast<client*>(d);
    c->serial = serial;
    ++c->dones;
}
void mgr_finished(void*, zwlr_output_manager_v1*) {}
const zwlr_output_manager_v1_listener manager_listener = {mgr_head, mgr_done, mgr_finished};

void out_geometry(void* d, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                  const char*, int32_t t) {
    static_cast<wl_out*>(d)->transform = t;
}
void out_mode(void* d, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        static_cast<wl_out*>(d)->w = w;
        static_cast<wl_out*>(d)->h = h;
    }
}
void out_done(void*, wl_output*) {}
void out_scale(void* d, wl_output*, int32_t s) { static_cast<wl_out*>(d)->scale = s; }
void out_name(void* d, wl_output*, const char* n) { static_cast<wl_out*>(d)->name = n; }
void out_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {out_geometry, out_mode, out_done,
                                            out_scale,    out_name, out_description};
void xo_position(void* d, zxdg_output_v1*, int32_t x, int32_t y) {
    static_cast<wl_out*>(d)->lx = x;
    static_cast<wl_out*>(d)->ly = y;
}
void xo_size(void* d, zxdg_output_v1*, int32_t w, int32_t h) {
    static_cast<wl_out*>(d)->lw = w;
    static_cast<wl_out*>(d)->lh = h;
}
void xo_done(void*, zxdg_output_v1*) {}
void xo_name(void*, zxdg_output_v1*, const char*) {}
void xo_description(void*, zxdg_output_v1*, const char*) {}
const zxdg_output_v1_listener xdg_output_listener = {xo_position, xo_size, xo_done, xo_name,
                                                     xo_description};

void bind_out(client& c, wl_out& o) {
    o.output = static_cast<wl_output*>(wl_registry_bind(c.registry, o.global, &wl_output_interface, 4));
    wl_output_add_listener(o.output, &output_listener, &o);
    if (c.xom) {
        o.xo = zxdg_output_manager_v1_get_xdg_output(c.xom, o.output);
        zxdg_output_v1_add_listener(o.xo, &xdg_output_listener, &o);
    }
}

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    if (!std::strcmp(iface, zwlr_output_manager_v1_interface.name)) {
        c->manager = static_cast<zwlr_output_manager_v1*>(
            wl_registry_bind(r, name, &zwlr_output_manager_v1_interface, version < 4 ? version : 4));
        zwlr_output_manager_v1_add_listener(c->manager, &manager_listener, c);
    } else if (!std::strcmp(iface, zxdg_output_manager_v1_interface.name)) {
        c->xom = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, 3));
    } else if (!std::strcmp(iface, wl_output_interface.name)) {
        auto* o = new wl_out{};
        o->global = name;
        c->outs.push_back(o);
        if (c->xom)
            bind_out(*c, *o);
    }
}
void on_global_remove(void* data, wl_registry*, uint32_t name) {
    for (auto* o : static_cast<client*>(data)->outs)
        if (o->global == name)
            o->removed = true;
}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

bool connect(client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    if (wl_display_roundtrip(c.display) < 0)
        return false;
    for (auto* o : c.outs)
        if (!o->output)
            bind_out(c, *o);
    return wl_display_roundtrip(c.display) >= 0 && c.manager;
}

head_info* head_named(client& c, const char* name) {
    for (auto* h : c.heads)
        if (!h->finished && h->name == name)
            return h;
    return nullptr;
}

wl_out* out_named(client& c, const char* name) {
    for (auto* o : c.outs)
        if (!o->removed && o->name == name)
            return o;
    return nullptr;
}

mode_info* mode_of(head_info* h, int w, int hh, int refresh = -1) {
    for (auto* m : h->modes)
        if (!m->finished && m->w == w && m->h == hh && (refresh < 0 || m->refresh == refresh))
            return m;
    return nullptr;
}

struct result {
    int state = -1; // 0 succeeded, 1 failed, 2 cancelled
};
void cfg_succeeded(void* d, zwlr_output_configuration_v1*) { static_cast<result*>(d)->state = 0; }
void cfg_failed(void* d, zwlr_output_configuration_v1*) { static_cast<result*>(d)->state = 1; }
void cfg_cancelled(void* d, zwlr_output_configuration_v1*) { static_cast<result*>(d)->state = 2; }
const zwlr_output_configuration_v1_listener cfg_listener = {cfg_succeeded, cfg_failed, cfg_cancelled};

/// One head's part of a configuration.
struct spec {
    const char* name = nullptr;
    bool enable = true;
    int mode_w = 0, mode_h = 0, mode_refresh = -1;  // set_mode with that mode
    int custom_w = 0, custom_h = 0, custom_refresh = 0; // set_custom_mode
    bool position = false;
    int x = 0, y = 0;
    int transform = -1;
    double scale = 0.0;
    int adaptive = -1;
};

zwlr_output_configuration_v1* build(client& c, const std::vector<spec>& specs, uint32_t serial,
                                    result& r) {
    zwlr_output_configuration_v1* cfg = zwlr_output_manager_v1_create_configuration(c.manager, serial);
    zwlr_output_configuration_v1_add_listener(cfg, &cfg_listener, &r);
    for (const spec& s : specs) {
        head_info* h = head_named(c, s.name);
        if (!h)
            continue;
        if (!s.enable) {
            zwlr_output_configuration_v1_disable_head(cfg, h->head);
            continue;
        }
        zwlr_output_configuration_head_v1* ch = zwlr_output_configuration_v1_enable_head(cfg, h->head);
        if (s.mode_w > 0)
            if (mode_info* m = mode_of(h, s.mode_w, s.mode_h, s.mode_refresh))
                zwlr_output_configuration_head_v1_set_mode(ch, m->mode);
        if (s.custom_w > 0)
            zwlr_output_configuration_head_v1_set_custom_mode(ch, s.custom_w, s.custom_h, s.custom_refresh);
        if (s.position)
            zwlr_output_configuration_head_v1_set_position(ch, s.x, s.y);
        if (s.transform >= 0)
            zwlr_output_configuration_head_v1_set_transform(ch, s.transform);
        if (s.scale > 0.0)
            zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(s.scale));
        if (s.adaptive >= 0)
            zwlr_output_configuration_head_v1_set_adaptive_sync(ch, static_cast<uint32_t>(s.adaptive));
    }
    return cfg;
}

} // namespace

int main() {
    char dir[] = "/tmp/lumen-outmgmt-XXXXXX";
    if (!mkdtemp(dir)) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    const std::string cfg_path = std::string(dir) + "/outputs.toml";
    ::setenv("LUMEN_OUTPUT_CONFIG", cfg_path.c_str(), 1);

    using scan_builder::connector;
    using scan_builder::mode;
    lx::drm::connector_scan scan{};
    connector(scan, 44, "Virtual-1", true, {mode(1024, 768), mode(800, 600), mode(640, 480)},
              {"Lumen", "Test Panel", "SN1"});
    connector(scan, 53, "Virtual-2", false);

    std::string saved;
    {
        lx::compositor::config cfg{};
        cfg.socket_name = k_socket;
        cfg.target_fps = 30.0;
        cfg.virtual_outputs = "800x600";
        lx::compositor::compositor comp{cfg};
        if (auto started = comp.start(); !started) {
            std::fprintf(stderr, "compositor start failed\n");
            return 1;
        }
        std::atomic<bool> pump{true};
        std::mutex dispatch_lock;
        std::thread pump_thread([&] {
            lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
            while (pump.load(std::memory_order_acquire)) {
                {
                    std::lock_guard<std::mutex> hold(dispatch_lock);
                    (void)comp.wayland().dispatch(10);
                    comp.run_posted_ui_tasks();
                    comp.wayland().flush();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        const auto ui = [&](auto fn) {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
            fn();
        };
        auto& outs = comp.outputs();
        ui([&] { comp.simulate_connector_scan(scan); });
        for (int i = 0; i < 200 && outs.count() < 2; ++i) {
            ui([&] { comp.run_posted_ui_tasks(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        client c{};
        const bool up = connect(c);
        check(up, "zwlr_output_manager_v1 v4 is advertised and binds");
        if (!up) {
            pump.store(false);
            pump_thread.join();
            return 1;
        }
        (void)wl_display_roundtrip(c.display);
        head_info* v1 = head_named(c, "Virtual-1");
        head_info* hl = head_named(c, "HEADLESS-1");
        check(c.heads.size() == 2 && v1 && hl && c.dones >= 1 && c.serial != 0,
              "two heads (the virtual output and the monitor), then done with a serial");
        if (v1) {
            check(v1->enabled && v1->modes.size() == 3 && v1->current && v1->current->w == 1024 &&
                      v1->current->preferred && v1->scale == 1.0 && v1->x == 800 && v1->y == 0,
                  "Virtual-1: three modes, 1024x768 (preferred) current, at 800,0, scale 1");
            check(v1->make == "Lumen" &&
                      v1->description == "Lumen Test Panel SN1 (Virtual-1)",
                  "make and description (wlroots' format)");
        }
        // A roundtrip and enough settle for an apply's answer to come back.
        const auto wait_result = [&](result& r) {
            for (int i = 0; i < 300 && r.state < 0; ++i) {
                (void)wl_display_roundtrip(c.display);
                if (r.state < 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            (void)wl_display_roundtrip(c.display);
            return r.state;
        };

        // ── apply: mode, position, scale ──
        {
            result r{};
            auto* cfg_obj = build(c,
                                  {{.name = "HEADLESS-1"},
                                   {.name = "Virtual-1", .mode_w = 800, .mode_h = 600,
                                    .position = true, .x = 0, .y = 600, .scale = 1.25}},
                                  c.serial, r);
            zwlr_output_configuration_v1_apply(cfg_obj);
            const uint32_t before = c.serial;
            const int st = wait_result(r);
            zwlr_output_configuration_v1_destroy(cfg_obj);
            check(st == 0, "apply (800x600 at 0,600 scale 1.25): succeeded");
            check(v1 && v1->current && v1->current->w == 800 && v1->x == 0 && v1->y == 600 &&
                      v1->scale == 1.25 && c.serial != before,
                  "the head shows it (current_mode, position, scale), with a new serial");
            wl_out* o = out_named(c, "Virtual-1");
            check(o && o->w == 800 && o->h == 600 && o->lx == 0 && o->ly == 600 && o->lw == 640 &&
                      o->lh == 480,
                  "wl_output and xdg-output follow (logical 640x480 at 0,600)");
        }
        // ── restating the same state: nothing changes ──
        {
            const uint32_t before = c.serial;
            const unsigned dones = c.dones;
            result r{};
            auto* cfg_obj = build(c,
                                  {{.name = "HEADLESS-1"},
                                   {.name = "Virtual-1", .mode_w = 800, .mode_h = 600,
                                    .position = true, .x = 0, .y = 600, .scale = 1.25}},
                                  c.serial, r);
            zwlr_output_configuration_v1_apply(cfg_obj);
            const int st = wait_result(r);
            zwlr_output_configuration_v1_destroy(cfg_obj);
            check(st == 0 && c.serial == before && c.dones == dones,
                  "applying the current state: succeeded, no event, same serial (kanshi at login)");
        }
        // ── test: answered, nothing changes ──
        {
            result r{};
            auto* cfg_obj = build(c,
                                  {{.name = "HEADLESS-1"},
                                   {.name = "Virtual-1", .mode_w = 640, .mode_h = 480}},
                                  c.serial, r);
            zwlr_output_configuration_v1_test(cfg_obj);
            const int st = wait_result(r);
            zwlr_output_configuration_v1_destroy(cfg_obj);
            check(st == 0 && v1 && v1->current && v1->current->w == 800,
                  "test of 640x480: succeeded, and the mode is still 800x600");
        }
        // ── failed ──
        {
            result r{};
            auto* cfg_obj = build(c, {{.name = "HEADLESS-1", .enable = false},
                                      {.name = "Virtual-1", .enable = false}},
                                  c.serial, r);
            zwlr_output_configuration_v1_apply(cfg_obj);
            const int st = wait_result(r);
            zwlr_output_configuration_v1_destroy(cfg_obj);
            check(st == 1 && outs.count() == 2, "every head disabled: failed, nothing changed");
            result r2{};
            auto* cfg2 = build(c, {{.name = "HEADLESS-1"}, {.name = "Virtual-1", .adaptive = 1}},
                               c.serial, r2);
            zwlr_output_configuration_v1_apply(cfg2);
            const int st2 = wait_result(r2);
            zwlr_output_configuration_v1_destroy(cfg2);
            check(st2 == 1, "adaptive sync on a head without vrr_capable: failed");
        }
        // ── cancelled ──
        {
            result r{};
            auto* cfg_obj = build(c, {{.name = "HEADLESS-1"}, {.name = "Virtual-1"}}, c.serial - 1, r);
            zwlr_output_configuration_v1_apply(cfg_obj);
            const int st = wait_result(r);
            zwlr_output_configuration_v1_destroy(cfg_obj);
            check(st == 2, "a stale serial: cancelled");
        }
        // ── disable and enable ──
        {
            result r{};
            auto* cfg_obj = build(c, {{.name = "HEADLESS-1"}, {.name = "Virtual-1", .enable = false}},
                                  c.serial, r);
            zwlr_output_configuration_v1_apply(cfg_obj);
            const int st = wait_result(r);
            zwlr_output_configuration_v1_destroy(cfg_obj);
            for (int i = 0; i < 100 && outs.count() != 1; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            (void)wl_display_roundtrip(c.display);
            check(st == 0 && outs.count() == 1 && !out_named(c, "Virtual-1") && v1 && !v1->finished &&
                      !v1->enabled,
                  "disabled: its wl_output goes, its head stays (enabled 0)");
            result r2{};
            auto* cfg2 = build(c, {{.name = "HEADLESS-1"}, {.name = "Virtual-1"}}, c.serial, r2);
            zwlr_output_configuration_v1_apply(cfg2);
            const int st2 = wait_result(r2);
            zwlr_output_configuration_v1_destroy(cfg2);
            for (int i = 0; i < 100 && !out_named(c, "Virtual-1"); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                (void)wl_display_roundtrip(c.display);
            }
            wl_out* o = out_named(c, "Virtual-1");
            check(st2 == 0 && v1 && v1->enabled && o && o->w == 800,
                  "enabled again: a new wl_output, with the mode it had (800x600)");
        }
        // ── custom mode and transform ──
        {
            result r{};
            auto* cfg_obj = build(c,
                                  {{.name = "HEADLESS-1"},
                                   {.name = "Virtual-1", .custom_w = 1000, .custom_h = 700,
                                    .custom_refresh = 50000, .transform = 1, .scale = 1.0}},
                                  c.serial, r);
            zwlr_output_configuration_v1_apply(cfg_obj);
            const int st = wait_result(r);
            zwlr_output_configuration_v1_destroy(cfg_obj);
            wl_out* o = out_named(c, "Virtual-1");
            check(st == 0 && v1 && v1->current && v1->current->w == 1000 && v1->current->h == 700 &&
                      v1->transform == 1,
                  "a custom mode 1000x700 and transform 90: the virtual mode object is current");
            check(o && o->w == 1000 && o->transform == 1 && o->lw == 700 && o->lh == 1000,
                  "wl_output.transform 90, and the logical size turns (700x1000)");
        }
        // ── hotplug: a new head, a configuration made before it is cancelled ──
        {
            result r{};
            const uint32_t old = c.serial;
            connector(scan, 53, "Virtual-2", true, {mode(1280, 720)});
            ui([&] { comp.simulate_connector_scan(scan); });
            for (int i = 0; i < 200 && outs.count() < 3; ++i) {
                ui([&] { comp.run_posted_ui_tasks(); });
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            for (int i = 0; i < 100 && !(head_named(c, "Virtual-2") && c.serial != old); ++i) {
                (void)wl_display_roundtrip(c.display);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            head_info* v2 = head_named(c, "Virtual-2");
            check(v2 && v2->enabled && c.serial != old, "a plugged monitor is a new head, new serial");
            auto* cfg_obj = build(c, {{.name = "HEADLESS-1"}, {.name = "Virtual-1"}}, old, r);
            zwlr_output_configuration_v1_apply(cfg_obj);
            check(wait_result(r) == 2, "a configuration of the old serial is cancelled (not an error)");
            zwlr_output_configuration_v1_destroy(cfg_obj);
            connector(scan, 53, "Virtual-2", false);
            ui([&] { comp.simulate_connector_scan(scan); });
            for (int i = 0; i < 200 && outs.count() > 2; ++i) {
                ui([&] { comp.run_posted_ui_tasks(); });
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            for (int i = 0; i < 100 && v2 && !v2->finished; ++i) {
                (void)wl_display_roundtrip(c.display);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(v2 && v2->finished, "unplugged: its head is finished");
        }

        // ── protocol errors, each on its own connection ──
        struct error_case {
            const char* what;
            const wl_interface* iface;
            uint32_t code;
            int kind;
        };
        const error_case cases[] = {
            {"enable_head twice: already_configured_head", &zwlr_output_configuration_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD, 0},
            {"a head left out: unconfigured_head", &zwlr_output_configuration_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_UNCONFIGURED_HEAD, 1},
            {"apply twice: already_used", &zwlr_output_configuration_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED, 2},
            {"set_scale twice: already_set", &zwlr_output_configuration_head_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET, 3},
            {"set_mode after set_custom_mode: already_set",
             &zwlr_output_configuration_head_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET, 4},
            {"another head's mode: invalid_mode", &zwlr_output_configuration_head_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_MODE, 5},
            {"custom mode 0x600: invalid_custom_mode", &zwlr_output_configuration_head_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_CUSTOM_MODE, 6},
            {"transform 9: invalid_transform", &zwlr_output_configuration_head_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_TRANSFORM, 7},
            {"scale 0: invalid_scale", &zwlr_output_configuration_head_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE, 8},
            {"adaptive sync 2: invalid_adaptive_sync_state",
             &zwlr_output_configuration_head_v1_interface,
             ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_ADAPTIVE_SYNC_STATE, 9},
        };
        for (const error_case& ec : cases) {
            client e{};
            if (!connect(e)) {
                check(false, ec.what);
                continue;
            }
            (void)wl_display_roundtrip(e.display);
            head_info* a = head_named(e, "HEADLESS-1");
            head_info* b = head_named(e, "Virtual-1");
            result r{};
            auto* cfg_obj = zwlr_output_manager_v1_create_configuration(e.manager, e.serial);
            zwlr_output_configuration_v1_add_listener(cfg_obj, &cfg_listener, &r);
            if (a && b) {
                switch (ec.kind) {
                case 0:
                    (void)zwlr_output_configuration_v1_enable_head(cfg_obj, a->head);
                    (void)zwlr_output_configuration_v1_enable_head(cfg_obj, a->head);
                    break;
                case 1:
                    (void)zwlr_output_configuration_v1_enable_head(cfg_obj, a->head);
                    zwlr_output_configuration_v1_apply(cfg_obj);
                    break;
                case 2:
                    (void)zwlr_output_configuration_v1_enable_head(cfg_obj, a->head);
                    (void)zwlr_output_configuration_v1_enable_head(cfg_obj, b->head);
                    zwlr_output_configuration_v1_apply(cfg_obj);
                    zwlr_output_configuration_v1_apply(cfg_obj);
                    break;
                default: {
                    (void)zwlr_output_configuration_v1_enable_head(cfg_obj, a->head);
                    auto* ch = zwlr_output_configuration_v1_enable_head(cfg_obj, b->head);
                    switch (ec.kind) {
                    case 3:
                        zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(1.0));
                        zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(2.0));
                        break;
                    case 4:
                        zwlr_output_configuration_head_v1_set_custom_mode(ch, 800, 600, 0);
                        zwlr_output_configuration_head_v1_set_mode(ch, b->modes[0]->mode);
                        break;
                    case 5:
                        zwlr_output_configuration_head_v1_set_mode(ch, a->modes[0]->mode);
                        break;
                    case 6: zwlr_output_configuration_head_v1_set_custom_mode(ch, 0, 600, 0); break;
                    case 7: zwlr_output_configuration_head_v1_set_transform(ch, 9); break;
                    case 8: zwlr_output_configuration_head_v1_set_scale(ch, 0); break;
                    case 9: zwlr_output_configuration_head_v1_set_adaptive_sync(ch, 2); break;
                    }
                    break;
                }
                }
            }
            for (int i = 0; i < 50 && wl_display_get_error(e.display) == 0; ++i)
                (void)wl_display_roundtrip(e.display);
            const wl_interface* iface = nullptr;
            uint32_t id = 0;
            const uint32_t code = wl_display_get_protocol_error(e.display, &iface, &id);
            check(wl_display_get_error(e.display) == EPROTO && iface == ec.iface && code == ec.code,
                  ec.what);
            wl_display_disconnect(e.display);
        }
        // The compositor survived every error.
        check(wl_display_roundtrip(c.display) >= 0, "the compositor and other clients are unaffected");

        // ── persistence: what was applied is written ──
        ui([&] { comp.save_output_config_now(); });
        if (FILE* f = std::fopen(cfg_path.c_str(), "rb")) {
            char buf[8192];
            const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
            saved.assign(buf, n);
            std::fclose(f);
        }
        check(saved.find("identifier = \"Lumen Test Panel SN1\"") != std::string::npos &&
                  saved.find("mode = \"1000x700@") != std::string::npos &&
                  saved.find("transform = \"90\"") != std::string::npos,
              "outputs.toml holds the monitor (custom mode, transform)");
        check(saved.find("[[setup]]") != std::string::npos &&
                  saved.find("position = [0, 600]") != std::string::npos,
              "and the setup with its position");

        for (auto* h : c.heads) {
            for (auto* m : h->modes)
                delete m;
            delete h;
        }
        wl_display_disconnect(c.display);
        pump.store(false, std::memory_order_release);
        pump_thread.join();
    }

    // ── a new compositor comes up as it was left ──
    {
        lx::compositor::config cfg{};
        cfg.socket_name = k_socket;
        cfg.target_fps = 30.0;
        cfg.virtual_outputs = "800x600";
        lx::compositor::compositor comp{cfg};
        if (auto started = comp.start(); !started) {
            std::fprintf(stderr, "second start failed\n");
            return 1;
        }
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        auto& outs = comp.outputs();
        connector(scan, 53, "Virtual-2", false);
        comp.simulate_connector_scan(scan);
        for (int i = 0; i < 200 && outs.count() < 2; ++i) {
            comp.run_posted_ui_tasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        int slot = -1;
        for (unsigned s = 0; s < lx::compositor::output_manager::k_max; ++s)
            if (outs.slot_live(s) && !std::strcmp(outs.slot_state(s).name, "Virtual-1"))
                slot = static_cast<int>(s);
        const bool ok = slot >= 0;
        const auto o = ok ? outs.slot_state(static_cast<unsigned>(slot)) : lx::compositor::output_state{};
        check(ok && o.mode.width == 1000 && o.mode.height == 700 && o.transform == 1 &&
                  o.position.x == 0 && o.position.y == 600 && !o.auto_position,
              "restored at connect: the custom mode, transform and position");
    }

    std::string cmd = std::string("rm -rf ") + dir;
    (void)std::system(cmd.c_str());
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
