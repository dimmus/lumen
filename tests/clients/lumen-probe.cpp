// Protocol probe for the app smoke: a deterministic xdg_popup and wlr-layer-shell client.
//
// Talks libwayland-client directly. It maps one plain toplevel; a left press inside it opens
// popup 0 with the positioner given on the command line, and a left press inside popup k
// opens popup k+1 (up to --chain), anchored at popup k's right edge. Every event of interest
// is one stdout line, so a smoke test can grep the sequence:
//
//   toplevel configure serial=<n> w=<w> h=<h>
//   pointer enter surface=<toplevel|popup<k>> serial=<n>
//   press surface=<toplevel|popup<k>> serial=<n>          release surface=<…>
//   popup open idx=<k> grab=<serial|->
//   popup configure idx=<k> x=<x> y=<y> w=<w> h=<h> serial=<n>
//   repositioned idx=0 token=<t>
//   popup_done idx=<k>
//   keyboard enter surface=<toplevel|popup<k>>             key code=<evdev> state=<0|1>
//
//   lumen-probe popup --size WxH --popup WxH --anchor-rect x,y,w,h --anchor N --gravity N
//                     --adjust BITS --offset x,y [--reactive] [--chain N] [--grab]
//                     [--grab-stale] [--reposition] [--exit-after S]
//
// --grab takes an explicit grab with the serial of the press that opened the popup;
// --grab-stale uses the serial of the `enter` before that press instead (a compositor that
// validates serials must deny it with popup_done). --reposition moves popup 0's anchor rect
// by (+20,+20) through xdg_popup.reposition 500 ms after it is configured.
//
// Popups are destroyed by the client as soon as popup_done reaches them, topmost first.
//
// Layer mode maps one layer surface instead of the toplevel (a left press on it opens a
// popup of it when --popup is given; that popup grabs with --grab):
//
//   lumen-probe layer --layer background|bottom|top|overlay --anchor top,bottom,left,right
//                     --size WxH --zone Z --margin t,r,b,l --kbd none|exclusive|on_demand
//                     [--edge top|bottom|left|right] [--ns NAME] [--popup WxH] [--grab]
//                     [--unmap-after S] [--exit-after S]
//
//   layer configure serial=<n> w=<w> h=<h>     layer closed     layer unmapped
//   keyboard enter surface=layer                key code=<evdev> state=<0|1>
//   protocol-error interface=<i> code=<c>      (then exit 3)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
// wayland-scanner names an argument `namespace`, a C++ keyword (Wayfire renames it the same
// way).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

namespace {

constexpr int k_max_popups = 8;

struct options {
    int width = 400, height = 300;
    int popup_w = 200, popup_h = 100;
    int ar_x = 10, ar_y = 10, ar_w = 50, ar_h = 20;
    unsigned anchor = 6, gravity = 8, adjust = 0;
    int off_x = 0, off_y = 0;
    bool reactive = false;
    int chain = 1;
    bool grab = false;
    bool grab_stale = false;
    bool reposition = false;
    double exit_after = 30.0;
    // layer mode
    bool layer_mode = false;
    unsigned layer = 2;
    unsigned layer_anchor = 0;
    int zone = 0;
    int margin[4]{}; // t, r, b, l
    unsigned kbd = 0;
    unsigned edge = 0;
    const char* ns = "lumen-probe";
    bool layer_popup = false;
    double unmap_after = 0.0;
};

struct surface_state {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    xdg_popup* popup = nullptr;
    wl_buffer* buffer = nullptr;
    int buffer_w = 0, buffer_h = 0;
    int pending_w = 0, pending_h = 0;
    int pending_x_y[2]{};
    uint32_t colour = 0xff808080;
    int index = -1; // -1: the toplevel
    bool done = false;
    bool configured = false;
};

struct app {
    options opt{};
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    surface_state top{};
    surface_state popups[k_max_popups]{};
    int popup_count = 0;
    surface_state* pointer_on = nullptr;
    uint32_t enter_serial = 0;
    bool closed = false;
    double reposition_at = 0.0;
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    zwlr_layer_shell_v1* layer_shell = nullptr;
    zwlr_layer_surface_v1* layer = nullptr;
#endif
    double unmap_at = 0.0;
    bool unmapped = false;
};

app g{};
volatile sig_atomic_t g_stop = 0;

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

const char* name_of(const surface_state* s, char* buf, size_t cap) {
    if (!s)
        return "none";
    if (s->index < 0)
        return g.opt.layer_mode ? "layer" : "toplevel";
    std::snprintf(buf, cap, "popup%d", s->index);
    return buf;
}

surface_state* find(wl_surface* surface) {
    if (g.top.surface == surface)
        return &g.top;
    for (int i = 0; i < g.popup_count; ++i)
        if (g.popups[i].surface == surface)
            return &g.popups[i];
    return nullptr;
}

/// A solid-colour XRGB buffer of w x h, in its own memfd pool.
wl_buffer* make_buffer(int w, int h, uint32_t colour) {
    const int stride = w * 4;
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(h);
    const int fd = memfd_create("lumen-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0)
        return nullptr;
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(data);
        for (size_t i = 0; i < size / 4; ++i)
            px[i] = colour;
        munmap(data, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int>(size));
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

void draw(surface_state& s, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (!s.buffer || s.buffer_w != w || s.buffer_h != h) {
        if (s.buffer)
            wl_buffer_destroy(s.buffer);
        s.buffer = make_buffer(w, h, s.colour);
        s.buffer_w = w;
        s.buffer_h = h;
    }
    wl_surface_attach(s.surface, s.buffer, 0, 0);
    wl_surface_damage_buffer(s.surface, 0, 0, w, h);
    wl_surface_commit(s.surface);
}

// ── xdg_wm_base / xdg_surface / xdg_toplevel ──

void on_ping(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener wm_base_listener = {on_ping};

void on_xdg_configure(void* data, xdg_surface* xs, uint32_t serial) {
    auto* s = static_cast<surface_state*>(data);
    xdg_surface_ack_configure(xs, serial);
    if (s->index < 0) {
        const int w = s->pending_w > 0 ? s->pending_w : g.opt.width;
        const int h = s->pending_h > 0 ? s->pending_h : g.opt.height;
        std::printf("toplevel configure serial=%u w=%d h=%d\n", serial, w, h);
        draw(*s, w, h);
    } else {
        std::printf("popup configure idx=%d x=%d y=%d w=%d h=%d serial=%u\n", s->index,
                    s->pending_x_y[0], s->pending_x_y[1], s->pending_w, s->pending_h, serial);
        if (!s->configured && s->index == 0 && g.opt.reposition)
            g.reposition_at = now_s() + 0.5;
        draw(*s, s->pending_w, s->pending_h);
    }
    s->configured = true;
}
const xdg_surface_listener xdg_listener = {on_xdg_configure};

void on_toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* s = static_cast<surface_state*>(data);
    s->pending_w = w;
    s->pending_h = h;
}
void on_toplevel_close(void*, xdg_toplevel*) { g.closed = true; }
void on_toplevel_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void on_toplevel_caps(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener toplevel_listener = {on_toplevel_configure, on_toplevel_close,
                                                 on_toplevel_bounds, on_toplevel_caps};

// ── xdg_popup ──

void destroy_finished_popups() {
    // Topmost first: the only order the protocol allows.
    while (g.popup_count > 0 && g.popups[g.popup_count - 1].done) {
        surface_state& p = g.popups[g.popup_count - 1];
        xdg_popup_destroy(p.popup);
        xdg_surface_destroy(p.xdg);
        wl_surface_destroy(p.surface);
        if (p.buffer)
            wl_buffer_destroy(p.buffer);
        if (g.pointer_on == &p)
            g.pointer_on = nullptr;
        p = {};
        --g.popup_count;
    }
}

void on_popup_configure(void* data, xdg_popup*, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* s = static_cast<surface_state*>(data);
    s->pending_x_y[0] = x;
    s->pending_x_y[1] = y;
    s->pending_w = w;
    s->pending_h = h;
}
void on_popup_done(void* data, xdg_popup*) {
    auto* s = static_cast<surface_state*>(data);
    std::printf("popup_done idx=%d\n", s->index);
    s->done = true;
    destroy_finished_popups();
}
void on_repositioned(void* data, xdg_popup*, uint32_t token) {
    std::printf("repositioned idx=%d token=%u\n", static_cast<surface_state*>(data)->index, token);
}
const xdg_popup_listener popup_listener = {on_popup_configure, on_popup_done, on_repositioned};

xdg_positioner* positioner_for(int index, int dx, int dy) {
    xdg_positioner* p = xdg_wm_base_create_positioner(g.wm_base);
    xdg_positioner_set_size(p, g.opt.popup_w, g.opt.popup_h);
    if (index == 0) {
        xdg_positioner_set_anchor_rect(p, g.opt.ar_x + dx, g.opt.ar_y + dy, g.opt.ar_w, g.opt.ar_h);
        xdg_positioner_set_anchor(p, g.opt.anchor);
        xdg_positioner_set_gravity(p, g.opt.gravity);
        xdg_positioner_set_constraint_adjustment(p, g.opt.adjust);
        xdg_positioner_set_offset(p, g.opt.off_x, g.opt.off_y);
        if (g.opt.reactive && xdg_wm_base_get_version(g.wm_base) >= 3)
            xdg_positioner_set_reactive(p);
    } else {
        // Nested: at the parent popup's right edge, top-aligned (a submenu).
        const surface_state& parent = g.popups[index - 1];
        const int pw = parent.pending_w > 0 ? parent.pending_w : g.opt.popup_w;
        xdg_positioner_set_anchor_rect(p, 0, 0, pw, 20);
        xdg_positioner_set_anchor(p, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
        xdg_positioner_set_gravity(p, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            p, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                   XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    }
    return p;
}

void open_popup(xdg_surface* parent, uint32_t press_serial) {
    if (g.popup_count >= k_max_popups)
        return;
    const int index = g.popup_count++;
    surface_state& s = g.popups[index];
    s = {};
    s.index = index;
    s.colour = index % 2 ? 0xffd98a3d : 0xff3dd98a;
    s.surface = wl_compositor_create_surface(g.compositor);
    s.xdg = xdg_wm_base_get_xdg_surface(g.wm_base, s.surface);
    xdg_surface_add_listener(s.xdg, &xdg_listener, &s);
    xdg_positioner* pos = positioner_for(index, 0, 0);
    s.popup = xdg_surface_get_popup(s.xdg, parent, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(s.popup, &popup_listener, &s);
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    // A layer surface's popup: created without a parent, adopted before its initial commit.
    if (index == 0 && g.layer)
        zwlr_layer_surface_v1_get_popup(g.layer, s.popup);
#endif
    uint32_t grab_serial = 0;
    if (g.opt.grab || g.opt.grab_stale) {
        grab_serial = g.opt.grab_stale ? g.enter_serial : press_serial;
        xdg_popup_grab(s.popup, g.seat, grab_serial);
        std::printf("popup open idx=%d grab=%u\n", index, grab_serial);
    } else {
        std::printf("popup open idx=%d grab=-\n", index);
    }
    wl_surface_commit(s.surface);
}

// ── seat ──

void on_pointer_enter(void*, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t,
                      wl_fixed_t) {
    g.pointer_on = find(surface);
    g.enter_serial = serial;
    char buf[16];
    std::printf("pointer enter surface=%s serial=%u\n", name_of(g.pointer_on, buf, sizeof(buf)),
                serial);
}
void on_pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) {
    g.pointer_on = nullptr;
    std::printf("pointer leave\n");
}
void on_pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void on_pointer_button(void*, wl_pointer*, uint32_t serial, uint32_t, uint32_t button,
                       uint32_t state) {
    char buf[16];
    const char* where = name_of(g.pointer_on, buf, sizeof(buf));
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        std::printf("release surface=%s button=%u\n", where, button);
        return;
    }
    std::printf("press surface=%s button=%u serial=%u\n", where, button, serial);
    if (button != 0x110 || !g.pointer_on)
        return;
    if (g.pointer_on == &g.top && g.popup_count == 0 &&
        (!g.opt.layer_mode || g.opt.layer_popup)) {
        open_popup(g.top.xdg, serial);
    } else if (g.pointer_on->index >= 0 && g.pointer_on->index == g.popup_count - 1 &&
               g.popup_count < g.opt.chain && !g.pointer_on->done) {
        open_popup(g.pointer_on->xdg, serial);
    }
}
void on_pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void on_pointer_frame(void*, wl_pointer*) { std::fflush(stdout); }
void on_axis_source(void*, wl_pointer*, uint32_t) {}
void on_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void on_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
const wl_pointer_listener pointer_listener = {on_pointer_enter, on_pointer_leave,
                                              on_pointer_motion, on_pointer_button,
                                              on_pointer_axis, on_pointer_frame,
                                              on_axis_source, on_axis_stop, on_axis_discrete};

void on_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
void on_key_enter(void*, wl_keyboard*, uint32_t, wl_surface* surface, wl_array*) {
    char buf[16];
    std::printf("keyboard enter surface=%s\n", name_of(find(surface), buf, sizeof(buf)));
}
void on_key_leave(void*, wl_keyboard*, uint32_t, wl_surface*) { std::printf("keyboard leave\n"); }
void on_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    std::printf("key code=%u state=%u\n", key, state);
}
void on_mods(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void on_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener keyboard_listener = {on_keymap, on_key_enter, on_key_leave,
                                                on_key, on_mods, on_repeat};

void on_caps(void*, wl_seat* seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &pointer_listener, nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.keyboard) {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &keyboard_listener, nullptr);
    }
}
void on_seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = {on_caps, on_seat_name};

#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
void bind_layer_shell(wl_registry* r, uint32_t name, const char* iface, uint32_t version);
#endif

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        g.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        g.shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        g.wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(r, name, &xdg_wm_base_interface, version < 3 ? version : 3));
    else if (std::strcmp(iface, wl_seat_interface.name) == 0)
        g.seat = static_cast<wl_seat*>(
            wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5));
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    else
        bind_layer_shell(r, name, iface, version);
#endif
}
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
void bind_layer_shell(wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
        g.layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, version < 5 ? version : 5));
}

void on_layer_configure(void*, zwlr_layer_surface_v1* l, uint32_t serial, uint32_t w,
                        uint32_t h) {
    zwlr_layer_surface_v1_ack_configure(l, serial);
    std::printf("layer configure serial=%u w=%u h=%u\n", serial, w, h);
    if (g.unmapped)
        return;
    const int dw = w ? static_cast<int>(w) : g.opt.width;
    const int dh = h ? static_cast<int>(h) : g.opt.height;
    if (!g.top.configured && g.opt.unmap_after > 0.0)
        g.unmap_at = now_s() + g.opt.unmap_after;
    g.top.configured = true;
    draw(g.top, dw, dh);
}
void on_layer_closed(void*, zwlr_layer_surface_v1*) {
    std::printf("layer closed\n");
    g.closed = true;
}
const zwlr_layer_surface_v1_listener layer_listener = {on_layer_configure, on_layer_closed};

unsigned anchor_bits(const char* list) {
    unsigned bits = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", list);
    for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ",")) {
        if (!std::strcmp(tok, "top")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        else if (!std::strcmp(tok, "bottom")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        else if (!std::strcmp(tok, "left")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        else if (!std::strcmp(tok, "right")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        else bits |= static_cast<unsigned>(std::atoi(tok));
    }
    return bits;
}

unsigned layer_value(const char* v) {
    if (!std::strcmp(v, "background")) return 0;
    if (!std::strcmp(v, "bottom")) return 1;
    if (!std::strcmp(v, "top")) return 2;
    if (!std::strcmp(v, "overlay")) return 3;
    return static_cast<unsigned>(std::atoi(v));
}

unsigned kbd_value(const char* v) {
    if (!std::strcmp(v, "none")) return 0;
    if (!std::strcmp(v, "exclusive")) return 1;
    if (!std::strcmp(v, "on_demand")) return 2;
    return static_cast<unsigned>(std::atoi(v));
}
#endif

void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

bool parse_pair(const char* s, int& a, int& b, char sep) {
    char* end = nullptr;
    a = static_cast<int>(std::strtol(s, &end, 10));
    if (!end || *end != sep)
        return false;
    b = static_cast<int>(std::strtol(end + 1, &end, 10));
    return end && *end == '\0';
}

int usage() {
    std::fprintf(stderr,
                 "usage: lumen-probe popup [--size WxH] [--popup WxH] [--anchor-rect x,y,w,h]\n"
                 "         [--anchor N] [--gravity N] [--adjust BITS] [--offset x,y]\n"
                 "         [--reactive] [--chain N] [--grab] [--grab-stale] [--reposition]\n"
                 "         [--exit-after S]\n"
                 "       lumen-probe layer [--layer L] [--anchor top,bottom,left,right] [--size WxH]\n"
                 "         [--zone Z] [--margin t,r,b,l] [--kbd none|exclusive|on_demand]\n"
                 "         [--edge E] [--ns NAME] [--popup WxH] [--grab] [--unmap-after S]\n"
                 "         [--exit-after S]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2 || (std::strcmp(argv[1], "popup") != 0 && std::strcmp(argv[1], "layer") != 0))
        return usage();
    options& o = g.opt;
    o.layer_mode = std::strcmp(argv[1], "layer") == 0;
#if !defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    if (o.layer_mode) {
        std::fprintf(stderr, "lumen-probe: built without wlr-layer-shell\n");
        return 2;
    }
#endif
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(a, "--size") && v && parse_pair(v, o.width, o.height, 'x')) ++i;
        else if (!std::strcmp(a, "--popup") && v && parse_pair(v, o.popup_w, o.popup_h, 'x')) {
            o.layer_popup = true;
            ++i;
        }
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
        else if (!std::strcmp(a, "--layer") && v) { o.layer = layer_value(v); ++i; }
        else if (o.layer_mode && !std::strcmp(a, "--anchor") && v) { o.layer_anchor = anchor_bits(v); ++i; }
        else if (!std::strcmp(a, "--zone") && v) { o.zone = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--margin") && v &&
                 std::sscanf(v, "%d,%d,%d,%d", &o.margin[0], &o.margin[1], &o.margin[2],
                             &o.margin[3]) == 4) ++i;
        else if (!std::strcmp(a, "--kbd") && v) { o.kbd = kbd_value(v); ++i; }
        else if (!std::strcmp(a, "--edge") && v) { o.edge = anchor_bits(v); ++i; }
        else if (!std::strcmp(a, "--ns") && v) { o.ns = v; ++i; }
        else if (!std::strcmp(a, "--unmap-after") && v) { o.unmap_after = std::atof(v); ++i; }
#endif
        else if (!std::strcmp(a, "--anchor-rect") && v &&
                 std::sscanf(v, "%d,%d,%d,%d", &o.ar_x, &o.ar_y, &o.ar_w, &o.ar_h) == 4) ++i;
        else if (!std::strcmp(a, "--anchor") && v) { o.anchor = static_cast<unsigned>(std::atoi(v)); ++i; }
        else if (!std::strcmp(a, "--gravity") && v) { o.gravity = static_cast<unsigned>(std::atoi(v)); ++i; }
        else if (!std::strcmp(a, "--adjust") && v) { o.adjust = static_cast<unsigned>(std::atoi(v)); ++i; }
        else if (!std::strcmp(a, "--offset") && v && parse_pair(v, o.off_x, o.off_y, ',')) ++i;
        else if (!std::strcmp(a, "--reactive")) o.reactive = true;
        else if (!std::strcmp(a, "--chain") && v) { o.chain = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--grab")) o.grab = true;
        else if (!std::strcmp(a, "--grab-stale")) o.grab_stale = true;
        else if (!std::strcmp(a, "--reposition")) o.reposition = true;
        else if (!std::strcmp(a, "--exit-after") && v) { o.exit_after = std::atof(v); ++i; }
        else return usage();
    }
    if (o.chain < 1)
        o.chain = 1;
    if (o.chain > k_max_popups)
        o.chain = k_max_popups;

    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        std::fprintf(stderr, "lumen-probe: cannot connect to the Wayland display\n");
        return 1;
    }
    wl_registry* registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(g.display);
    if (!g.compositor || !g.shm || !g.wm_base || !g.seat) {
        std::fprintf(stderr, "lumen-probe: missing a required global\n");
        return 1;
    }
    xdg_wm_base_add_listener(g.wm_base, &wm_base_listener, nullptr);
    wl_seat_add_listener(g.seat, &seat_listener, nullptr);

    g.top.colour = 0xff5060a0;
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    if (o.layer_mode) {
        if (!g.layer_shell) {
            std::fprintf(stderr, "lumen-probe: no zwlr_layer_shell_v1\n");
            return 1;
        }
        g.top.colour = 0xff3a7a50;
        g.top.surface = wl_compositor_create_surface(g.compositor);
        g.layer = zwlr_layer_shell_v1_get_layer_surface(g.layer_shell, g.top.surface, nullptr,
                                                        o.layer, o.ns);
        zwlr_layer_surface_v1_add_listener(g.layer, &layer_listener, nullptr);
        zwlr_layer_surface_v1_set_size(g.layer, static_cast<uint32_t>(o.width),
                                       static_cast<uint32_t>(o.height));
        zwlr_layer_surface_v1_set_anchor(g.layer, o.layer_anchor);
        zwlr_layer_surface_v1_set_exclusive_zone(g.layer, o.zone);
        zwlr_layer_surface_v1_set_margin(g.layer, o.margin[0], o.margin[1], o.margin[2],
                                         o.margin[3]);
        zwlr_layer_surface_v1_set_keyboard_interactivity(g.layer, o.kbd);
        if (o.edge && zwlr_layer_surface_v1_get_version(g.layer) >= 5)
            zwlr_layer_surface_v1_set_exclusive_edge(g.layer, o.edge);
        wl_surface_commit(g.top.surface);
    }
#endif
    if (!o.layer_mode) {
        g.top.surface = wl_compositor_create_surface(g.compositor);
        g.top.xdg = xdg_wm_base_get_xdg_surface(g.wm_base, g.top.surface);
        xdg_surface_add_listener(g.top.xdg, &xdg_listener, &g.top);
        g.top.toplevel = xdg_surface_get_toplevel(g.top.xdg);
        xdg_toplevel_add_listener(g.top.toplevel, &toplevel_listener, &g.top);
        xdg_toplevel_set_title(g.top.toplevel, "lumen-probe");
        xdg_toplevel_set_app_id(g.top.toplevel, "lumen-probe");
        wl_surface_commit(g.top.surface);
    }

    const double deadline = now_s() + o.exit_after;
    int rc = 0;
    while (!g_stop && !g.closed && now_s() < deadline) {
        if (g.unmap_at > 0.0 && now_s() >= g.unmap_at && !g.unmapped) {
            // Null buffer: the layer surface unmaps and goes back to before its initial
            // commit; this probe stays unmapped until it exits.
            g.unmapped = true;
            wl_surface_attach(g.top.surface, nullptr, 0, 0);
            wl_surface_commit(g.top.surface);
            std::printf("layer unmapped\n");
        }
        if (g.reposition_at > 0.0 && now_s() >= g.reposition_at && g.popup_count > 0 &&
            !g.popups[0].done) {
            g.reposition_at = 0.0;
            if (xdg_wm_base_get_version(g.wm_base) >= 3) {
                xdg_positioner* pos = positioner_for(0, 20, 20);
                xdg_popup_reposition(g.popups[0].popup, pos, 42);
                xdg_positioner_destroy(pos);
                std::printf("reposition idx=0 token=42\n");
            }
        }
        while (wl_display_prepare_read(g.display) != 0)
            wl_display_dispatch_pending(g.display);
        if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g.display);
            rc = 1;
            break;
        }
        pollfd pfd{wl_display_get_fd(g.display), POLLIN, 0};
        const int ready = poll(&pfd, 1, 50);
        if (ready > 0) {
            if (wl_display_read_events(g.display) < 0) {
                rc = 1;
                break;
            }
        } else {
            wl_display_cancel_read(g.display);
        }
        if (wl_display_dispatch_pending(g.display) < 0) {
            rc = 1;
            break;
        }
    }
    if (wl_display_get_error(g.display) == EPROTO) {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        const uint32_t code = wl_display_get_protocol_error(g.display, &iface, &id);
        std::printf("protocol-error interface=%s code=%u\n", iface ? iface->name : "?", code);
        rc = 3;
    }
    wl_display_disconnect(g.display);
    return rc;
}
