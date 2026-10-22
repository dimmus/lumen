// The pointer protocols of design-app-compat-capture Part C, in-process against a real
// compositor (one libwayland-client connection per case; a protocol error is fatal to its
// connection): pointer-constraints-unstable-v1 (already_constrained; a lock that holds the
// pointer still while relative motion flows and no wl_pointer.motion is sent; focus loss,
// Super+Escape and re-arming of persistent vs oneshot; the cursor-hint warp on unlock; a
// confinement clipping motion and following a new region), relative-pointer-unstable-v1
// (accelerated and unaccelerated deltas, microsecond time, only to the pointer focus) and
// pointer-gestures-unstable-v1 v3 (swipe, pinch, hold latched to the client focused at
// begin). A region staged after a synchronized subsurface's cached commit applies with its
// own commit's update, not with the earlier one its parent applies.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_POINTER_CONSTRAINTS_UNSTABLE_V1) && \
    defined(LUMEN_PROTOCOL_RELATIVE_POINTER_UNSTABLE_V1) &&                                       \
    defined(LUMEN_PROTOCOL_POINTER_GESTURES_UNSTABLE_V1) && defined(LUMEN_PROTOCOL_EXT_SESSION_LOCK_V1)
#include "xdg-shell-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#define LUMEN_TEST_PART_C 1
#endif
#endif

import lx.foundation;
import lx.runtime;
import lx.input;
import lx.compositor;

#if !defined(LUMEN_TEST_PART_C)

int main() {
    std::printf("SKIP: needs libwayland and the pointer protocols' glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-pointer-protocols";

int g_failed = 0;
int g_ran = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    struct global {
        std::string name;
        uint32_t version;
    };
    std::vector<global> globals;
};

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    c->globals.push_back({iface, version});
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < 6 ? version : 6));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        c->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (std::strcmp(iface, wl_seat_interface.name) == 0)
        c->seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 5));
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

bool connect(client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    return wl_display_roundtrip(c.display) >= 0 && wl_display_roundtrip(c.display) >= 0;
}

uint32_t global_version(const client& c, const char* iface) {
    for (const auto& g : c.globals)
        if (g.name == iface)
            return g.version;
    return 0;
}

void* bind_global(client& c, const wl_interface* iface, uint32_t version) {
    struct found {
        const wl_interface* iface;
        uint32_t name = 0;
        uint32_t version = 0;
    } f{iface};
    static const wl_registry_listener listener = {
        [](void* data, wl_registry*, uint32_t name, const char* i, uint32_t v) {
            auto* fd = static_cast<found*>(data);
            if (std::strcmp(i, fd->iface->name) == 0) {
                fd->name = name;
                fd->version = v;
            }
        },
        [](void*, wl_registry*, uint32_t) {}};
    wl_registry* r = wl_display_get_registry(c.display);
    wl_registry_add_listener(r, &listener, &f);
    (void)wl_display_roundtrip(c.display);
    if (!f.name)
        return nullptr;
    return wl_registry_bind(r, f.name, iface, version < f.version ? version : f.version);
}

struct outcome {
    const char* interface = "none";
    uint32_t code = 0;
};

outcome finish(client& c) {
    (void)wl_display_roundtrip(c.display);
    outcome o{};
    if (wl_display_get_error(c.display) == EPROTO) {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        o.code = wl_display_get_protocol_error(c.display, &iface, &id);
        o.interface = iface ? iface->name : "[destroyed]";
    }
    wl_display_disconnect(c.display);
    c.display = nullptr;
    return o;
}

void expect(const char* name, const char* interface, uint32_t code,
            const std::function<void(client&)>& body) {
    ++g_ran;
    client c{};
    if (!connect(c)) {
        std::printf("  FAIL  %s — could not connect\n", name);
        ++g_failed;
        return;
    }
    body(c);
    const outcome o = finish(c);
    const bool ok = std::strcmp(o.interface, interface) == 0 && o.code == code;
    std::printf("  %s  %s — got %s/%u, want %s/%u\n", ok ? "PASS" : "FAIL", name, o.interface,
                o.code, interface, code);
    if (!ok)
        ++g_failed;
}

void check(const char* name, bool ok, const std::string& detail) {
    ++g_ran;
    std::printf("  %s  %s — %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    if (void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); p != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(p);
        for (int i = 0; i < w * h; ++i)
            px[i] = 0xFF336699u;
        munmap(p, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

struct configured {
    uint32_t serial = 0;
};
const xdg_surface_listener xdg_listener = {
    [](void* data, xdg_surface*, uint32_t serial) { static_cast<configured*>(data)->serial = serial; }};

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    configured cfg{};
};

void map_window(client& c, window& w, const char* app_id, int width = 300, int height = 200) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w.cfg);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(w.xdg, w.cfg.serial);
    wl_surface_attach(w.surface, make_buffer(c, width, height), 0, 0);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
}

lx::toplevel_id window_id(const char* app_id, lx::rect2i* geometry = nullptr) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::toplevel_id out{};
    lx::rect2i g{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id()) {
            out = r.id;
            g = r.geometry;
        }
    });
    if (geometry)
        *geometry = g;
    return out;
}

void activate(lx::toplevel_id id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    g_comp->windows().activate(id);
    g_comp->wayland().flush();
}

void inject_move(double x, double y) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::pointer_motion_event m{};
    m.x = x;
    m.y = y;
    g_comp->inject_pointer_motion(m);
    g_comp->wayland().flush();
}

void inject_rel(double dx, double dy, double dxu, double dyu, unsigned long long t_us = 0) {
    std::lock_guard<std::mutex> hold(*g_lock);
    g_comp->inject_pointer_relative(dx, dy, dxu, dyu, t_us);
    g_comp->wayland().flush();
}

void inject_key(unsigned code, bool pressed) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::key_event k{};
    k.evdev_keycode = code;
    k.pressed = pressed;
    g_comp->inject_key(k);
    g_comp->wayland().flush();
}

void super_escape() {
    constexpr unsigned k_meta = 125, k_esc = 1;
    inject_key(k_meta, true);
    inject_key(k_esc, true);
    inject_key(k_esc, false);
    inject_key(k_meta, false);
}

void inject_gesture(lx::input::gesture_event::kind kind, lx::input::gesture_event::phase phase,
                    unsigned fingers = 3, double dx = 0, double dy = 0, double scale = 1,
                    double rotation = 0, bool cancelled = false) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::gesture_event e{};
    e.type = kind;
    e.stage = phase;
    e.fingers = fingers;
    e.dx = dx;
    e.dy = dy;
    e.scale = scale;
    e.rotation = rotation;
    e.cancelled = cancelled;
    e.time_ms = 1234;
    g_comp->inject_gesture(e);
    g_comp->wayland().flush();
}

double seat_x() {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->seats().seat().pointer_x();
}
double seat_y() {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->seats().seat().pointer_y();
}

void settle(client& c) {
    (void)wl_display_roundtrip(c.display);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)wl_display_roundtrip(c.display);
}

// ── what the client hears ──

struct pointer_log {
    unsigned enters = 0;
    unsigned leaves = 0;
    unsigned motions = 0;
    double last_x = -1;
    double last_y = -1;
    unsigned frames = 0;
    unsigned locked = 0;
    unsigned unlocked = 0;
    unsigned confined = 0;
    unsigned unconfined = 0;
    unsigned relative = 0;
    double rdx = 0, rdy = 0, rdxu = 0, rdyu = 0;
    unsigned long long rtime = 0;
    std::vector<std::string> gestures;
};

const wl_pointer_listener ptr_listener = {
    [](void* d, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
        auto* l = static_cast<pointer_log*>(d);
        l->enters++;
        l->last_x = wl_fixed_to_double(x);
        l->last_y = wl_fixed_to_double(y);
    },
    [](void* d, wl_pointer*, uint32_t, wl_surface*) { static_cast<pointer_log*>(d)->leaves++; },
    [](void* d, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto* l = static_cast<pointer_log*>(d);
        l->motions++;
        l->last_x = wl_fixed_to_double(x);
        l->last_y = wl_fixed_to_double(y);
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
    [](void* d, wl_pointer*) { static_cast<pointer_log*>(d)->frames++; },
    [](void*, wl_pointer*, uint32_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t) {},
    [](void*, wl_pointer*, uint32_t, int32_t) {},
    nullptr,
    nullptr};

const zwp_locked_pointer_v1_listener locked_listener = {
    [](void* d, zwp_locked_pointer_v1*) { static_cast<pointer_log*>(d)->locked++; },
    [](void* d, zwp_locked_pointer_v1*) { static_cast<pointer_log*>(d)->unlocked++; }};
const zwp_confined_pointer_v1_listener confined_listener = {
    [](void* d, zwp_confined_pointer_v1*) { static_cast<pointer_log*>(d)->confined++; },
    [](void* d, zwp_confined_pointer_v1*) { static_cast<pointer_log*>(d)->unconfined++; }};
const zwp_relative_pointer_v1_listener relative_listener = {
    [](void* d, zwp_relative_pointer_v1*, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy,
       wl_fixed_t dxu, wl_fixed_t dyu) {
        auto* l = static_cast<pointer_log*>(d);
        l->relative++;
        l->rdx += wl_fixed_to_double(dx);
        l->rdy += wl_fixed_to_double(dy);
        l->rdxu += wl_fixed_to_double(dxu);
        l->rdyu += wl_fixed_to_double(dyu);
        l->rtime = (static_cast<unsigned long long>(hi) << 32) | lo;
    }};

struct pointer_client {
    client c{};
    pointer_log log{};
    wl_pointer* pointer = nullptr;
    zwp_pointer_constraints_v1* constraints = nullptr;
    zwp_relative_pointer_manager_v1* relative = nullptr;
    window w{};
    lx::rect2i geometry{};
};

bool open_pointer_client(pointer_client& p, const char* app_id) {
    if (!connect(p.c) || !p.c.seat)
        return false;
    p.pointer = wl_seat_get_pointer(p.c.seat);
    wl_pointer_add_listener(p.pointer, &ptr_listener, &p.log);
    p.constraints = static_cast<zwp_pointer_constraints_v1*>(
        bind_global(p.c, &zwp_pointer_constraints_v1_interface, 1));
    p.relative = static_cast<zwp_relative_pointer_manager_v1*>(
        bind_global(p.c, &zwp_relative_pointer_manager_v1_interface, 1));
    if (!p.constraints || !p.relative)
        return false;
    auto* rel = zwp_relative_pointer_manager_v1_get_relative_pointer(p.relative, p.pointer);
    zwp_relative_pointer_v1_add_listener(rel, &relative_listener, &p.log);
    map_window(p.c, p.w, app_id);
    settle(p.c);
    (void)window_id(app_id, &p.geometry);
    return p.geometry.width > 0;
}

/// A point inside the window, `fx`/`fy` of the way across.
void move_into(pointer_client& p, double fx = 0.5, double fy = 0.5) {
    inject_move(p.geometry.x + p.geometry.width * fx, p.geometry.y + p.geometry.height * fy);
    settle(p.c);
}

void park_pointer() {
    inject_move(2, 2);
}

// ── cases ──

void globals_case() {
    client c{};
    if (!connect(c)) {
        check("globals", false, "connect");
        return;
    }
    const uint32_t pc = global_version(c, "zwp_pointer_constraints_v1");
    const uint32_t rp = global_version(c, "zwp_relative_pointer_manager_v1");
    const uint32_t pg = global_version(c, "zwp_pointer_gestures_v1");
    check("globals: constraints v1, relative pointer v1, gestures v3", pc == 1 && rp == 1 && pg == 3,
          "constraints " + std::to_string(pc) + " relative " + std::to_string(rp) + " gestures " +
              std::to_string(pg));
    wl_display_disconnect(c.display);
}

void error_cases() {
    expect("constraints: a lock and a confinement on one surface", "zwp_pointer_constraints_v1",
           ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED, [](client& c) {
               auto* m = static_cast<zwp_pointer_constraints_v1*>(
                   bind_global(c, &zwp_pointer_constraints_v1_interface, 1));
               wl_pointer* p = wl_seat_get_pointer(c.seat);
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               (void)zwp_pointer_constraints_v1_lock_pointer(
                   m, s, p, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
               (void)zwp_pointer_constraints_v1_confine_pointer(
                   m, s, p, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
           });
    expect("constraints: a new one after the first was destroyed; the surface goes first",
           "none", 0, [](client& c) {
               auto* m = static_cast<zwp_pointer_constraints_v1*>(
                   bind_global(c, &zwp_pointer_constraints_v1_interface, 1));
               wl_pointer* p = wl_seat_get_pointer(c.seat);
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               zwp_locked_pointer_v1_destroy(zwp_pointer_constraints_v1_lock_pointer(
                   m, s, p, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT));
               auto* conf = zwp_pointer_constraints_v1_confine_pointer(
                   m, s, p, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
               // An unknown lifetime is taken as oneshot (no error defined for it).
               wl_surface* s2 = wl_compositor_create_surface(c.compositor);
               (void)zwp_pointer_constraints_v1_lock_pointer(m, s2, p, nullptr, 7);
               wl_surface_destroy(s);
               // Inert after its surface went: requests are harmless.
               zwp_confined_pointer_v1_set_region(conf, nullptr);
               zwp_confined_pointer_v1_destroy(conf);
           });
}

void lock_cases() {
    pointer_client p{};
    if (!open_pointer_client(p, "lock-app")) {
        check("lock: setup", false, "connect / globals / map");
        return;
    }
    park_pointer();
    settle(p.c);
    auto* lock = zwp_pointer_constraints_v1_lock_pointer(
        p.constraints, p.w.surface, p.pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(lock, &locked_listener, &p.log);
    settle(p.c);
    check("lock: not active while the pointer is outside the surface", p.log.locked == 0, "");
    move_into(p);
    check("lock: active once the pointer enters the focused window", p.log.locked == 1,
          "locked " + std::to_string(p.log.locked));
    const double x0 = seat_x(), y0 = seat_y();
    const unsigned motions0 = p.log.motions, frames0 = p.log.frames;
    for (int i = 0; i < 5; ++i)
        inject_rel(10.0, 4.0, 7.0, 3.0, 1'000'000ull + static_cast<unsigned long long>(i));
    settle(p.c);
    check("lock: relative motion arrives (accelerated and unaccelerated, µs time)",
          p.log.relative >= 5 && std::fabs(p.log.rdx - 50) < 0.01 && std::fabs(p.log.rdy - 20) < 0.01 &&
              std::fabs(p.log.rdxu - 35) < 0.01 && std::fabs(p.log.rdyu - 15) < 0.01 &&
              p.log.rtime == 1'000'004ull,
          "events " + std::to_string(p.log.relative) + " dx " + std::to_string(p.log.rdx) + " dxu " +
              std::to_string(p.log.rdxu) + " t " + std::to_string(p.log.rtime));
    check("lock: the pointer does not move and no wl_pointer.motion is sent (frames are)",
          seat_x() == x0 && seat_y() == y0 && p.log.motions == motions0 && p.log.frames >= frames0 + 5,
          "moved " + std::to_string(seat_x() - x0) + "," + std::to_string(seat_y() - y0) + " motions " +
              std::to_string(p.log.motions - motions0));

    // Another window takes the keyboard: unlocked; the pointer moves again.
    pointer_client other{};
    const bool other_ok = open_pointer_client(other, "lock-other");
    settle(p.c);
    check("lock: deactivated when its window loses the keyboard", other_ok && p.log.unlocked == 1,
          "unlocked " + std::to_string(p.log.unlocked));
    // Persistent: back to the window (keyboard), pointer over it → locked again.
    activate(window_id("lock-app", &p.geometry));
    move_into(p, 0.3, 0.3);
    settle(p.c);
    check("lock: persistent — holds again when focus and pointer return", p.log.locked == 2,
          "locked " + std::to_string(p.log.locked));

    // Super+Escape releases; it does not re-arm while the pointer stays.
    super_escape();
    settle(p.c);
    const unsigned before_moves = p.log.motions;
    inject_rel(5, 0, 5, 0);
    settle(p.c);
    check("lock: Super+Escape releases it, the pointer moves again",
          p.log.unlocked == 2 && p.log.locked == 2 && p.log.motions > before_moves,
          "unlocked " + std::to_string(p.log.unlocked) + " locked " + std::to_string(p.log.locked));
    // Leave the surface and come back: re-armed.
    park_pointer();
    settle(p.c);
    move_into(p, 0.6, 0.6);
    check("lock: after leaving and re-entering it holds again", p.log.locked == 3,
          "locked " + std::to_string(p.log.locked));

    // The session locks (from the request on): unlocked; logind's Unlock ends the lock and
    // the persistent constraint holds again on the window that gets both foci back.
    {
        client locker{};
        auto* mgr = connect(locker) ? static_cast<ext_session_lock_manager_v1*>(
                                          bind_global(locker, &ext_session_lock_manager_v1_interface, 1))
                                    : nullptr;
        if (mgr) {
            (void)ext_session_lock_manager_v1_lock(mgr);
            (void)wl_display_roundtrip(locker.display);
            settle(p.c);
            check("lock: deactivated when the session locks", p.log.unlocked == 3,
                  "unlocked " + std::to_string(p.log.unlocked));
            const unsigned rel_locked = p.log.relative;
            inject_rel(4, 4, 4, 4);
            settle(p.c);
            check("relative: nothing reaches a window while the session is locked",
                  p.log.relative == rel_locked, "");
            {
                std::lock_guard<std::mutex> hold(*g_lock);
                g_comp->inject_logind_unlock();
                g_comp->wayland().flush();
            }
            (void)wl_display_roundtrip(locker.display);
            settle(p.c);
            move_into(p, 0.62, 0.62);
            check("lock: holds again after the session is unlocked", p.log.locked == 4,
                  "locked " + std::to_string(p.log.locked));
        } else {
            check("lock: session lock", false, "ext_session_lock_manager_v1 not bound");
        }
        if (locker.display)
            wl_display_disconnect(locker.display);
    }

    // The cursor hint: applied with the next commit; destroying the lock warps there.
    zwp_locked_pointer_v1_set_cursor_position_hint(lock, wl_fixed_from_int(40), wl_fixed_from_int(25));
    wl_surface_commit(p.w.surface);
    settle(p.c);
    zwp_locked_pointer_v1_destroy(lock);
    settle(p.c);
    // Window geometry may include no offset here (no CSD): the surface origin is the geometry's.
    const bool warped = std::fabs(p.log.last_x - 40) < 0.01 && std::fabs(p.log.last_y - 25) < 0.01;
    check("lock: destroying it warps the pointer to the cursor hint (wl_pointer.motion)", warped,
          "client position " + std::to_string(p.log.last_x) + "," + std::to_string(p.log.last_y));

    // Oneshot: spent by its first deactivation; a new constraint may then be made.
    auto* once = zwp_pointer_constraints_v1_lock_pointer(
        p.constraints, p.w.surface, p.pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    pointer_log once_log{};
    zwp_locked_pointer_v1_add_listener(once, &locked_listener, &once_log);
    settle(p.c);
    activate(window_id("lock-other"));
    settle(p.c);
    activate(window_id("lock-app", &p.geometry));
    move_into(p, 0.4, 0.4);
    settle(p.c);
    check("lock: oneshot — locked, unlocked on focus loss, never again",
          once_log.locked == 1 && once_log.unlocked == 1,
          "locked " + std::to_string(once_log.locked) + " unlocked " + std::to_string(once_log.unlocked));
    auto* again = zwp_pointer_constraints_v1_confine_pointer(
        p.constraints, p.w.surface, p.pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    settle(p.c);
    check("lock: a spent oneshot does not block a new constraint",
          wl_display_get_error(p.c.display) == 0, "");
    zwp_confined_pointer_v1_destroy(again);
    zwp_locked_pointer_v1_destroy(once);
    settle(p.c);

    // Relative motion goes only to the pointer focus's client.
    const unsigned other_rel = other.log.relative;
    inject_rel(3, 3, 3, 3);
    settle(p.c);
    settle(other.c);
    check("relative: only the pointer focus's client hears it", other.log.relative == other_rel,
          "other got " + std::to_string(other.log.relative - other_rel));
    wl_display_disconnect(other.c.display);
    wl_display_disconnect(p.c.display);
    park_pointer();
}

void confine_cases() {
    pointer_client p{};
    if (!open_pointer_client(p, "confine-app")) {
        check("confine: setup", false, "connect / globals / map");
        return;
    }
    // Region 20,20 100x60 of the surface.
    wl_region* r = wl_compositor_create_region(p.c.compositor);
    wl_region_add(r, 20, 20, 100, 60);
    auto* conf = zwp_pointer_constraints_v1_confine_pointer(
        p.constraints, p.w.surface, p.pointer, r, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    wl_region_destroy(r);
    zwp_confined_pointer_v1_add_listener(conf, &confined_listener, &p.log);
    inject_move(p.geometry.x + 5, p.geometry.y + 5); // over the surface, outside the region
    settle(p.c);
    check("confine: not active outside its region", p.log.confined == 0, "");
    inject_move(p.geometry.x + 50, p.geometry.y + 50);
    settle(p.c);
    check("confine: active inside it", p.log.confined == 1, "confined " + std::to_string(p.log.confined));
    // A long motion right and down: clipped at the region's corner, relative motion in full.
    const unsigned rel0 = p.log.relative;
    const double rdx0 = p.log.rdx;
    inject_rel(400, 300, 400, 300);
    settle(p.c);
    const double ex = 120.0 - 1.0 / 256.0, ey = 80.0 - 1.0 / 256.0;
    check("confine: a motion out of the region stops at its edge; relative motion is whole",
          std::fabs(p.log.last_x - ex) < 0.01 && std::fabs(p.log.last_y - ey) < 0.01 &&
              p.log.relative == rel0 + 1 && std::fabs(p.log.rdx - rdx0 - 400) < 0.01,
          "client position " + std::to_string(p.log.last_x) + "," + std::to_string(p.log.last_y));
    // Sliding along the bottom edge.
    inject_rel(-30, 50, -30, 50);
    settle(p.c);
    check("confine: a motion into the edge slides along it",
          std::fabs(p.log.last_x - (ex - 30)) < 0.01 && std::fabs(p.log.last_y - ey) < 0.01,
          "client position " + std::to_string(p.log.last_x) + "," + std::to_string(p.log.last_y));
    // A new region that leaves the pointer outside: warped to the nearest point inside.
    wl_region* r2 = wl_compositor_create_region(p.c.compositor);
    wl_region_add(r2, 20, 20, 40, 30);
    zwp_confined_pointer_v1_set_region(conf, r2);
    wl_region_destroy(r2);
    wl_surface_commit(p.w.surface);
    settle(p.c);
    check("confine: a new region applied with the commit brings the pointer inside",
          std::fabs(p.log.last_x - (60 - 1.0 / 256.0)) < 0.01 &&
              std::fabs(p.log.last_y - (50 - 1.0 / 256.0)) < 0.01 && p.log.confined == 1 &&
              p.log.unconfined == 0,
          "client position " + std::to_string(p.log.last_x) + "," + std::to_string(p.log.last_y));
    // Destroyed: free again.
    zwp_confined_pointer_v1_destroy(conf);
    settle(p.c);
    inject_rel(200, 0, 200, 0);
    settle(p.c);
    check("confine: destroyed, the pointer leaves the region",
          p.log.last_x > 100 || p.log.leaves > 0, "x " + std::to_string(p.log.last_x));
    // Surface destroyed while confined: the object is told and goes inert.
    pointer_log l2{};
    move_into(p, 0.5, 0.5);
    auto* conf2 = zwp_pointer_constraints_v1_confine_pointer(
        p.constraints, p.w.surface, p.pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_confined_pointer_v1_add_listener(conf2, &confined_listener, &l2);
    settle(p.c);
    xdg_toplevel_destroy(p.w.toplevel);
    xdg_surface_destroy(p.w.xdg);
    wl_surface_destroy(p.w.surface);
    settle(p.c);
    check("confine: its surface destroyed — unconfined, then inert",
          l2.confined == 1 && l2.unconfined == 1 && wl_display_get_error(p.c.display) == 0,
          "confined " + std::to_string(l2.confined) + " unconfined " + std::to_string(l2.unconfined));
    zwp_confined_pointer_v1_destroy(conf2);
    settle(p.c);
    wl_display_disconnect(p.c.display);
    park_pointer();
}

// ── gestures ──

struct gesture_log {
    std::vector<std::string> events;
    wl_surface* surface = nullptr;
};

const zwp_pointer_gesture_swipe_v1_listener swipe_listener = {
    [](void* d, zwp_pointer_gesture_swipe_v1*, uint32_t, uint32_t, wl_surface* s, uint32_t fingers) {
        auto* g = static_cast<gesture_log*>(d);
        g->surface = s;
        g->events.push_back("swipe-begin " + std::to_string(fingers));
    },
    [](void* d, zwp_pointer_gesture_swipe_v1*, uint32_t, wl_fixed_t dx, wl_fixed_t dy) {
        static_cast<gesture_log*>(d)->events.push_back(
            "swipe-update " + std::to_string(wl_fixed_to_int(dx)) + "," + std::to_string(wl_fixed_to_int(dy)));
    },
    [](void* d, zwp_pointer_gesture_swipe_v1*, uint32_t, uint32_t, int32_t cancelled) {
        static_cast<gesture_log*>(d)->events.push_back("swipe-end " + std::to_string(cancelled));
    }};
const zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    [](void* d, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t, wl_surface*, uint32_t fingers) {
        static_cast<gesture_log*>(d)->events.push_back("pinch-begin " + std::to_string(fingers));
    },
    [](void* d, zwp_pointer_gesture_pinch_v1*, uint32_t, wl_fixed_t, wl_fixed_t, wl_fixed_t scale,
       wl_fixed_t rotation) {
        char b[64];
        std::snprintf(b, sizeof(b), "pinch-update %.2f %.1f", wl_fixed_to_double(scale),
                      wl_fixed_to_double(rotation));
        static_cast<gesture_log*>(d)->events.push_back(b);
    },
    [](void* d, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t, int32_t cancelled) {
        static_cast<gesture_log*>(d)->events.push_back("pinch-end " + std::to_string(cancelled));
    }};
const zwp_pointer_gesture_hold_v1_listener hold_listener = {
    [](void* d, zwp_pointer_gesture_hold_v1*, uint32_t, uint32_t, wl_surface*, uint32_t fingers) {
        static_cast<gesture_log*>(d)->events.push_back("hold-begin " + std::to_string(fingers));
    },
    [](void* d, zwp_pointer_gesture_hold_v1*, uint32_t, uint32_t, int32_t cancelled) {
        static_cast<gesture_log*>(d)->events.push_back("hold-end " + std::to_string(cancelled));
    }};

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v)
        out += (out.empty() ? "" : "; ") + s;
    return out;
}

void gesture_cases() {
    using K = lx::input::gesture_event::kind;
    using P = lx::input::gesture_event::phase;
    pointer_client p{};
    if (!open_pointer_client(p, "gesture-app")) {
        check("gestures: setup", false, "connect / globals / map");
        return;
    }
    auto* g = static_cast<zwp_pointer_gestures_v1*>(bind_global(p.c, &zwp_pointer_gestures_v1_interface, 3));
    if (!g) {
        check("gestures: bind", false, "not advertised");
        return;
    }
    gesture_log log{};
    zwp_pointer_gesture_swipe_v1_add_listener(zwp_pointer_gestures_v1_get_swipe_gesture(g, p.pointer),
                                              &swipe_listener, &log);
    zwp_pointer_gesture_pinch_v1_add_listener(zwp_pointer_gestures_v1_get_pinch_gesture(g, p.pointer),
                                              &pinch_listener, &log);
    zwp_pointer_gesture_hold_v1_add_listener(zwp_pointer_gestures_v1_get_hold_gesture(g, p.pointer),
                                             &hold_listener, &log);
    settle(p.c);
    // Pointer elsewhere: nothing.
    park_pointer();
    settle(p.c);
    inject_gesture(K::swipe, P::begin, 3);
    inject_gesture(K::swipe, P::update, 3, 5, 0);
    inject_gesture(K::swipe, P::end, 3);
    settle(p.c);
    check("gestures: none while the pointer is not on the client", log.events.empty(), joined(log.events));
    move_into(p);
    inject_gesture(K::swipe, P::begin, 3);
    inject_gesture(K::swipe, P::update, 3, 12, -4);
    // Latched: the pointer leaves mid-gesture, the rest still arrives.
    park_pointer();
    inject_gesture(K::swipe, P::update, 3, 1, 1);
    inject_gesture(K::swipe, P::end, 3);
    settle(p.c);
    check("gestures: a swipe latched to the client focused at begin",
          joined(log.events) == "swipe-begin 3; swipe-update 12,-4; swipe-update 1,1; swipe-end 0" &&
              log.surface == p.w.surface,
          joined(log.events));
    log.events.clear();
    move_into(p);
    inject_gesture(K::pinch, P::begin, 2);
    inject_gesture(K::pinch, P::update, 2, 0, 0, 1.5, 10);
    inject_gesture(K::pinch, P::end, 2, 0, 0, 1, 0, true);
    inject_gesture(K::hold, P::begin, 4);
    inject_gesture(K::hold, P::end, 4, 0, 0, 1, 0, true);
    settle(p.c);
    check("gestures: pinch (scale, rotation, cancelled) and hold (v3)",
          joined(log.events) == "pinch-begin 2; pinch-update 1.50 10.0; pinch-end 1; hold-begin 4; hold-end 1",
          joined(log.events));
    zwp_pointer_gestures_v1_release(g);
    settle(p.c);
    check("gestures: release (v2) destroys the manager cleanly", wl_display_get_error(p.c.display) == 0, "");
    wl_display_disconnect(p.c.display);
    park_pointer();
}

} // namespace

/// A synchronized subsurface's commit waits for its parent's: state staged after that
/// commit (and not committed yet) must not ride along when the parent applies it.
void staged_region_case() {
    pointer_client p{};
    if (!open_pointer_client(p, "staged-app")) {
        check("staged region: setup", false, "connect / globals / map");
        return;
    }
    auto* sub = static_cast<wl_subcompositor*>(bind_global(p.c, &wl_subcompositor_interface, 1));
    if (!sub) {
        check("staged region: setup", false, "no wl_subcompositor");
        return;
    }
    wl_surface* child = wl_compositor_create_surface(p.c.compositor);
    wl_subsurface* ss = wl_subcompositor_get_subsurface(sub, child, p.w.surface);
    wl_subsurface_set_position(ss, 0, 0);
    wl_surface_attach(child, make_buffer(p.c, 200, 150), 0, 0);
    wl_surface_commit(child);
    wl_surface_commit(p.w.surface);
    settle(p.c);
    wl_region* r1 = wl_compositor_create_region(p.c.compositor);
    wl_region_add(r1, 20, 20, 100, 60);
    auto* conf = zwp_pointer_constraints_v1_confine_pointer(
        p.constraints, child, p.pointer, r1, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    wl_region_destroy(r1);
    zwp_confined_pointer_v1_add_listener(conf, &confined_listener, &p.log);
    inject_move(p.geometry.x + 50, p.geometry.y + 50);
    settle(p.c);
    check("staged region: confined in its first region", p.log.confined == 1,
          "confined " + std::to_string(p.log.confined));
    // C1 is cached (synchronized); set_region comes after it, uncommitted; the parent's
    // commit applies C1 — and only C1.
    wl_surface_commit(child);
    wl_region* r2 = wl_compositor_create_region(p.c.compositor);
    wl_region_add(r2, 20, 20, 40, 30);
    zwp_confined_pointer_v1_set_region(conf, r2);
    wl_region_destroy(r2);
    wl_surface_commit(p.w.surface);
    settle(p.c);
    // Inside the first region, outside the second: not clipped, not warped.
    inject_rel(30, 10, 30, 10);
    settle(p.c);
    check("staged region: an earlier update leaves a later-staged region pending",
          std::fabs(p.log.last_x - 80) < 0.01 && std::fabs(p.log.last_y - 60) < 0.01,
          "client position " + std::to_string(p.log.last_x) + "," + std::to_string(p.log.last_y));
    // Its own commit, applied by the parent's next one, installs it: brought inside.
    wl_surface_commit(child);
    wl_surface_commit(p.w.surface);
    settle(p.c);
    check("staged region: applied with the commit that carried it",
          std::fabs(p.log.last_x - (60 - 1.0 / 256.0)) < 0.01 &&
              std::fabs(p.log.last_y - (50 - 1.0 / 256.0)) < 0.01,
          "client position " + std::to_string(p.log.last_x) + "," + std::to_string(p.log.last_y));
    zwp_confined_pointer_v1_destroy(conf);
    wl_subsurface_destroy(ss);
    wl_surface_destroy(child);
    settle(p.c);
    park_pointer();
}

int main() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    std::atomic<bool> pump{true};
    std::mutex dispatch_lock;
    g_comp = &comp;
    g_lock = &dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int loop_fd = comp.wayland().event_fd();
        while (pump.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 10);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    globals_case();
    error_cases();
    lock_cases();
    confine_cases();
    staged_region_case();
    gesture_cases();

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
