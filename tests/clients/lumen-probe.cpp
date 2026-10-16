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
//
// Selection mode maps the toplevel and owns or reads a selection (clipboard or primary):
//
//   lumen-probe selection [--kind clipboard|primary] [--mimes a,b] [--data STR]
//                         [--set-on press|key|focus|never] [--stale-serial] [--reuse-source]
//                         [--receive MIME] [--exit-after S]
//
// A left press (or a key press, or the keyboard enter) sets the selection to a source
// offering --mimes, quoting that event's serial — or serial 1 with --stale-serial, which a
// compositor that checks serials must refuse; --reuse-source then sets the same source a
// second time (wl_data_device.error.used_source). The source answers `send` with --data.
// With --receive, every offer naming MIME is read and printed.
//
//   selection set serial=<n>              selection cancelled      selection send mime=<m>
//   selection offer mimes=[a,b]           selection offer none
//   selection received mime=<m> data="<d>"
//
// Drag-and-drop source mode: a left press on the toplevel starts a drag (wl_data_device
// .start_drag) of a source offering --mimes with --actions, quoting that press's serial — or
// the pointer enter's serial with --stale-serial, which a compositor must refuse. --icon
// gives it a WxH icon attached at (-W/2, -H/2) after start_drag; --null-source drags with no
// source (a drag inside this client); --second-window maps a second toplevel (app_id
// lumen-probe-2) that is a drop target like the target mode's; --exit-during-drag S exits
// abruptly S seconds after start_drag (client death mid-drag).
//
//   lumen-probe dnd-source [--mimes a,b] [--data STR] [--actions copy,move,ask] [--icon WxH]
//                          [--null-source] [--stale-serial] [--second-window]
//                          [--exit-during-drag S] [--exit-after S]
//
//   dnd source start serial=<n>           dnd source target mime=<m|nil>
//   dnd source action=<a>                 dnd source send mime=<m>
//   dnd source drop_performed             dnd source finished      dnd source cancelled
//
// Drag-and-drop target mode: maps the toplevel; every drag entering it is answered with
// accept(--accept, or NULL for none) and set_actions(--actions, --preferred); a drop is read
// (the accepted type) and finished, unless --no-finish.
//
//   lumen-probe dnd-target [--accept MIME|none] [--actions copy,move,ask] [--preferred A]
//                          [--no-finish] [--exit-after S]
//
//   dnd enter surface=<toplevel|second> x=<x> y=<y> offer=<id|nil> mimes=[a,b] source_actions=<a>
//   dnd motion x=<x> y=<y>                dnd action=<a>           dnd leave       dnd drop
//   dnd received data="<d>"               dnd finish
//
// Outputs mode: popup mode (a left press opens popup 0), plus the outputs. It binds wl_output
// (v4) and zxdg_output_manager_v1 (v3) — at once, or with --late S that many seconds after
// the toplevel's first configure — and reports what they say and every wl_surface.enter /
// leave of its toplevel and popups. --unmap-after S unmaps the toplevel (null attach).
//
//   lumen-probe outputs [--late S] [--unmap-after S] [popup options]
//
//   wl_output bind version=<v>
//   wl_output name=<n> mode=<w>x<h> refresh=<mHz> scale=<s> done=<k>
//   xdg_output position=<x>,<y> size=<w>x<h> name=<n> description=<d>
//   surface enter surface=<toplevel|popup<k>> output=<n>   surface leave …   toplevel unmapped
//
// Liveness, in any mode with a toplevel: --ignore-ping-for S leaves xdg_wm_base.ping
// unanswered for S seconds after the first configure, then answers the newest one held
// (a client that catches up); --freeze S stops dispatching for S seconds after the first
// frame; --ssd asks for a server-side frame (xdg-decoration) and --title T sets the title.
//
//   ping serial=<n> ignored               pong serial=<n> [late]
//   freeze s=<S>                          thaw
//
// Activation issuer mode: maps the toplevel; a left press on it requests an xdg-activation
// token quoting that press's serial (none with --no-serial) and writes it to --token-file
// (atomically: written beside it, then renamed).
//
//   lumen-probe activation-issuer --token-file F [--no-serial] [--exit-after S]
//
//   activation request serial=<n|none>   activation token=<first 8 chars>
//
// Activation target mode: maps the toplevel and presents a token for it — --token STR, or
// the contents of --token-file F once that file exists (polled). --early presents it before
// the initial commit (the window is not mapped yet; F must exist at start). Every configure
// also prints whether it carries the activated state.
//
//   lumen-probe activation-target (--token STR | --token-file F) [--early] [--exit-after S]
//
//   activation activate token=<first 8 chars> mapped=<0|1>
//   toplevel activated=<0|1>              keyboard enter surface=toplevel

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

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#if __has_include("xdg-output-unstable-v1-client-protocol.h")
#include "xdg-output-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_XDG_OUTPUT 1
#endif
#if __has_include("xdg-decoration-unstable-v1-client-protocol.h")
#include "xdg-decoration-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_DECORATION 1
#endif
#if __has_include("primary-selection-unstable-v1-client-protocol.h")
#include "primary-selection-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_PRIMARY_SELECTION 1
#endif
#if __has_include("xdg-activation-v1-client-protocol.h")
#include "xdg-activation-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_ACTIVATION 1
#endif
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
    // selection mode
    bool selection_mode = false;
    bool primary = false;
    const char* mimes = "text/plain;charset=utf-8,text/plain,UTF8_STRING";
    const char* data = "lumen-probe";
    enum class set_on { press, key, focus, never } trigger = set_on::press;
    bool stale_serial = false;
    bool reuse_source = false;
    const char* receive = nullptr;
    // drag-and-drop modes
    bool dnd_source_mode = false;
    bool dnd_target_mode = false;
    unsigned dnd_actions = 3;     // source actions (dnd-source), offer actions (dnd-target)
    unsigned dnd_preferred = 2;
    const char* dnd_accept = "text/plain";
    bool no_finish = false;
    int icon_w = 0, icon_h = 0;
    bool null_source = false;
    bool second_window = false;
    double exit_during_drag = -1.0;
    // outputs mode
    bool outputs_mode = false;
    double late_output = 0.0;
    // liveness
    double ignore_ping_for = 0.0;
    double freeze = 0.0;
    bool ssd = false;
    const char* title = "lumen-probe";
    // activation modes
    bool activation_issuer = false;
    bool activation_target = false;
    const char* token_file = nullptr;
    const char* token = nullptr;
    bool no_serial = false;
    bool early = false;
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
    // selection mode
    wl_data_device_manager* ddm = nullptr;
    wl_data_device* data_device = nullptr;
    wl_data_offer* offer = nullptr;
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    zwp_primary_selection_device_manager_v1* psm = nullptr;
    zwp_primary_selection_device_v1* primary_device = nullptr;
    zwp_primary_selection_offer_v1* primary_offer = nullptr;
#endif
    bool source_set = false;
    // drag-and-drop
    surface_state second{};
    wl_data_source* dnd_source = nullptr;
    bool drag_started = false;
    wl_surface* icon = nullptr;
    wl_buffer* icon_buffer = nullptr;
    wl_data_offer* dnd_offer = nullptr;
    unsigned dnd_source_actions = 0;
    bool dnd_dropped = false;
    bool dnd_accepted = false;
    char dnd_accepted_mime[256]{};
    double exit_at = 0.0;
    const char* recv_label = "selection";
    char offer_mimes[1024]{};
    bool offer_has_receive = false;
    int recv_fd = -1;
    char recv_buf[1024]{};
    size_t recv_len = 0;
    // outputs mode
    wl_registry* registry = nullptr;
    uint32_t output_global = 0;
    uint32_t output_version = 0;
    wl_output* output = nullptr;
    double bind_output_at = 0.0;
    char output_name[64]{};
    int output_w = 0, output_h = 0, output_refresh = 0, output_scale = 0;
    unsigned output_dones = 0;
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
    zxdg_output_manager_v1* xom = nullptr;
    zxdg_output_v1* xdg_output = nullptr;
#endif
    int xo_x = 0, xo_y = 0, xo_w = 0, xo_h = 0;
    char xo_name[64]{};
    char xo_description[128]{};
    bool xo_seen = false;
    // liveness
    double ignore_until = 0.0;
    uint32_t deferred_ping = 0;
    double freeze_at = 0.0;
#if defined(LUMEN_PROBE_HAS_DECORATION)
    zxdg_decoration_manager_v1* deco_manager = nullptr;
    zxdg_toplevel_decoration_v1* deco = nullptr;
#endif
    // activation
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    xdg_activation_v1* activation = nullptr;
#endif
    bool activated = false;     // the target presented its token
    bool token_requested = false;
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
    if (s->index == -2)
        return "second";
    if (s->index < 0)
        return g.opt.layer_mode ? "layer" : "toplevel";
    std::snprintf(buf, cap, "popup%d", s->index);
    return buf;
}

surface_state* find(wl_surface* surface) {
    if (g.top.surface == surface)
        return &g.top;
    if (g.second.surface && g.second.surface == surface)
        return &g.second;
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

// ── outputs mode: wl_output, xdg-output, wl_surface.enter/leave ──

void on_surface_enter(void*, wl_surface* surface, wl_output*) {
    char buf[16];
    std::printf("surface enter surface=%s output=%s\n", name_of(find(surface), buf, sizeof(buf)),
                g.output_name[0] ? g.output_name : "?");
}
void on_surface_leave(void*, wl_surface* surface, wl_output*) {
    char buf[16];
    std::printf("surface leave surface=%s output=%s\n", name_of(find(surface), buf, sizeof(buf)),
                g.output_name[0] ? g.output_name : "?");
}
void on_surface_scale(void*, wl_surface*, int32_t s) {
    std::printf("surface preferred_scale=%d\n", s);
}
void on_surface_transform(void*, wl_surface*, uint32_t t) {
    std::printf("surface preferred_transform=%u\n", t);
}
const wl_surface_listener surface_listener = {on_surface_enter, on_surface_leave,
                                              on_surface_scale, on_surface_transform};

void on_output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t,
                        const char*, const char*, int32_t) {}
void on_output_mode(void*, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        g.output_w = w;
        g.output_h = h;
        g.output_refresh = refresh;
    }
}
void on_output_done(void*, wl_output*) {
    ++g.output_dones;
    std::printf("wl_output name=%s mode=%dx%d refresh=%d scale=%d done=%u\n", g.output_name,
                g.output_w, g.output_h, g.output_refresh, g.output_scale, g.output_dones);
    if (g.xo_seen)
        std::printf("xdg_output position=%d,%d size=%dx%d name=%s description=%s\n", g.xo_x,
                    g.xo_y, g.xo_w, g.xo_h, g.xo_name, g.xo_description);
}
void on_output_scale(void*, wl_output*, int32_t s) { g.output_scale = s; }
void on_output_name(void*, wl_output*, const char* n) {
    std::snprintf(g.output_name, sizeof(g.output_name), "%s", n);
}
void on_output_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {on_output_geometry, on_output_mode,
                                            on_output_done,     on_output_scale,
                                            on_output_name,     on_output_description};

#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
void on_xo_position(void*, zxdg_output_v1*, int32_t x, int32_t y) {
    g.xo_x = x;
    g.xo_y = y;
    g.xo_seen = true;
}
void on_xo_size(void*, zxdg_output_v1*, int32_t w, int32_t h) {
    g.xo_w = w;
    g.xo_h = h;
}
void on_xo_done(void*, zxdg_output_v1*) {} // v3: wl_output.done closes the group
void on_xo_name(void*, zxdg_output_v1*, const char* n) {
    std::snprintf(g.xo_name, sizeof(g.xo_name), "%s", n);
}
void on_xo_description(void*, zxdg_output_v1*, const char* d) {
    std::snprintf(g.xo_description, sizeof(g.xo_description), "%s", d);
}
const zxdg_output_v1_listener xdg_output_listener = {on_xo_position, on_xo_size, on_xo_done,
                                                     on_xo_name, on_xo_description};
#endif

void bind_outputs() {
    if (g.output || !g.output_global)
        return;
    const uint32_t v = g.output_version < 4 ? g.output_version : 4;
    std::printf("wl_output bind version=%u\n", v);
    g.output = static_cast<wl_output*>(
        wl_registry_bind(g.registry, g.output_global, &wl_output_interface, v));
    wl_output_add_listener(g.output, &output_listener, nullptr);
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
    if (g.xom) {
        g.xdg_output = zxdg_output_manager_v1_get_xdg_output(g.xom, g.output);
        zxdg_output_v1_add_listener(g.xdg_output, &xdg_output_listener, nullptr);
    }
#endif
}

#if defined(LUMEN_PROBE_HAS_DECORATION)
void on_deco_configure(void*, zxdg_toplevel_decoration_v1*, uint32_t mode) {
    std::printf("decoration configure mode=%u\n", mode);
}
const zxdg_toplevel_decoration_v1_listener deco_listener = {on_deco_configure};
#endif

// ── xdg_wm_base / xdg_surface / xdg_toplevel ──

void on_ping(void*, xdg_wm_base* base, uint32_t serial) {
    if (g.opt.ignore_ping_for > 0.0) {
        // Not answered until the window has passed (counted from the first configure); the
        // newest ping is answered then, as a client that catches up would.
        if (g.ignore_until == 0.0 || now_s() < g.ignore_until) {
            g.deferred_ping = serial;
            std::printf("ping serial=%u ignored\n", serial);
            return;
        }
        std::printf("pong serial=%u\n", serial);
    }
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_base_listener = {on_ping};

void on_xdg_configure(void* data, xdg_surface* xs, uint32_t serial) {
    auto* s = static_cast<surface_state*>(data);
    xdg_surface_ack_configure(xs, serial);
    if (s->index < 0) {
        const int w = s->pending_w > 0 ? s->pending_w : g.opt.width;
        const int h = s->pending_h > 0 ? s->pending_h : g.opt.height;
        std::printf("toplevel configure serial=%u w=%d h=%d\n", serial, w, h);
        if (s == &g.top && !s->configured) {
            const double now = now_s();
            if (g.opt.ignore_ping_for > 0.0)
                g.ignore_until = now + g.opt.ignore_ping_for;
            if (g.opt.freeze > 0.0)
                g.freeze_at = now + 0.2; // after the first frame is on screen
            if (g.opt.outputs_mode && g.opt.late_output > 0.0)
                g.bind_output_at = now + g.opt.late_output;
            if (!g.opt.layer_mode && g.opt.unmap_after > 0.0)
                g.unmap_at = now + g.opt.unmap_after;
        }
        if (g.unmapped)
            return;
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

void on_toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array* states) {
    auto* s = static_cast<surface_state*>(data);
    s->pending_w = w;
    s->pending_h = h;
    if (g.opt.activation_issuer || g.opt.activation_target) {
        bool active = false;
        const auto* st = static_cast<const uint32_t*>(states->data);
        for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i)
            active = active || st[i] == XDG_TOPLEVEL_STATE_ACTIVATED;
        std::printf("toplevel activated=%d\n", active ? 1 : 0);
    }
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
    if (g.opt.outputs_mode)
        wl_surface_add_listener(s.surface, &surface_listener, nullptr);
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

// ── selection mode ──

void sel_send(const char* mime, int32_t fd) {
    std::printf("selection send mime=%s\n", mime);
    const size_t n = std::strlen(g.opt.data);
    (void)!write(fd, g.opt.data, n);
    close(fd);
}

void on_source_target(void*, wl_data_source*, const char*) {}
void on_source_send(void*, wl_data_source*, const char* mime, int32_t fd) { sel_send(mime, fd); }
void on_source_cancelled(void*, wl_data_source* src) {
    std::printf("selection cancelled\n");
    wl_data_source_destroy(src);
}
void on_source_drop(void*, wl_data_source*) {}
void on_source_finished(void*, wl_data_source*) {}
void on_source_action(void*, wl_data_source*, uint32_t) {}
const wl_data_source_listener source_listener = {on_source_target, on_source_send,
                                                 on_source_cancelled, on_source_drop,
                                                 on_source_finished, on_source_action};

#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
void on_psource_send(void*, zwp_primary_selection_source_v1*, const char* mime, int32_t fd) {
    sel_send(mime, fd);
}
void on_psource_cancelled(void*, zwp_primary_selection_source_v1* src) {
    std::printf("selection cancelled\n");
    zwp_primary_selection_source_v1_destroy(src);
}
const zwp_primary_selection_source_v1_listener psource_listener = {on_psource_send,
                                                                   on_psource_cancelled};
#endif

/// Sets the selection quoting `serial` (once; --reuse-source sets the same source twice).
void set_selection(uint32_t serial) {
    if (g.source_set || g.opt.trigger == options::set_on::never)
        return;
    g.source_set = true;
    if (g.opt.stale_serial)
        serial = 1;
    char list[512];
    std::snprintf(list, sizeof(list), "%s", g.opt.mimes);
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    if (g.opt.primary) {
        if (!g.primary_device)
            return;
        auto* src = zwp_primary_selection_device_manager_v1_create_source(g.psm);
        zwp_primary_selection_source_v1_add_listener(src, &psource_listener, nullptr);
        for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
            zwp_primary_selection_source_v1_offer(src, t);
        zwp_primary_selection_device_v1_set_selection(g.primary_device, src, serial);
        if (g.opt.reuse_source)
            zwp_primary_selection_device_v1_set_selection(g.primary_device, src, serial);
        std::printf("selection set serial=%u\n", serial);
        return;
    }
#endif
    if (!g.data_device)
        return;
    wl_data_source* src = wl_data_device_manager_create_data_source(g.ddm);
    wl_data_source_add_listener(src, &source_listener, nullptr);
    for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
        wl_data_source_offer(src, t);
    wl_data_device_set_selection(g.data_device, src, serial);
    if (g.opt.reuse_source)
        wl_data_device_set_selection(g.data_device, src, serial);
    std::printf("selection set serial=%u\n", serial);
}

/// Mimes of the offer being announced (data_offer, then one `offer` each).
void note_offer_mime(const char* mime) {
    const size_t used = std::strlen(g.offer_mimes);
    std::snprintf(g.offer_mimes + used, sizeof(g.offer_mimes) - used, "%s%s", used ? "," : "",
                  mime);
    if (g.opt.receive && std::strcmp(mime, g.opt.receive) == 0)
        g.offer_has_receive = true;
}

/// A selection arrived (null: none). Prints it and, with --receive, starts the read.
template <typename Receive>
void on_selection_offer(bool present, Receive receive) {
    if (!present) {
        std::printf("selection offer none\n");
        return;
    }
    std::printf("selection offer mimes=[%s]\n", g.offer_mimes);
    if (!g.opt.receive || !g.offer_has_receive || g.recv_fd >= 0)
        return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    receive(fds[1]);
    close(fds[1]);
    g.recv_fd = fds[0];
    g.recv_len = 0;
}

void on_offer_offer(void*, wl_data_offer*, const char* mime) { note_offer_mime(mime); }
void on_offer_source_actions(void*, wl_data_offer*, uint32_t a) { g.dnd_source_actions = a; }
void on_offer_action(void*, wl_data_offer* o, uint32_t a) {
    if (o == g.dnd_offer)
        std::printf("dnd action=%u\n", a);
}
const wl_data_offer_listener offer_listener = {on_offer_offer, on_offer_source_actions,
                                               on_offer_action};

void on_dd_data_offer(void*, wl_data_device*, wl_data_offer* o) {
    g.offer_mimes[0] = '\0';
    g.offer_has_receive = false;
    g.dnd_source_actions = 0;
    wl_data_offer_add_listener(o, &offer_listener, nullptr);
}

// ── drag-and-drop: target side ──

/// The offer's type list contains `mime` exactly.
bool offer_lists(const char* mime) {
    char list[1024];
    std::snprintf(list, sizeof(list), "%s", g.offer_mimes);
    for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
        if (std::strcmp(t, mime) == 0)
            return true;
    return false;
}

void dnd_forget_offer() {
    if (g.dnd_offer)
        wl_data_offer_destroy(g.dnd_offer);
    g.dnd_offer = nullptr;
    g.dnd_dropped = false;
    g.dnd_accepted = false;
}

void on_dd_enter(void*, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x,
                 wl_fixed_t y, wl_data_offer* o) {
    surface_state* on = find(surface);
    char buf[16];
    std::printf("dnd enter surface=%s x=%.0f y=%.0f offer=%s mimes=[%s] source_actions=%u\n",
                name_of(on, buf, sizeof(buf)), wl_fixed_to_double(x), wl_fixed_to_double(y),
                o ? "obj" : "nil", o ? g.offer_mimes : "", o ? g.dnd_source_actions : 0u);
    if (g.dnd_offer && g.dnd_offer != o && !g.dnd_dropped)
        dnd_forget_offer();
    g.dnd_offer = o;
    g.dnd_dropped = false;
    if (!o)
        return;
    // The source probe's own toplevel is not a target; its second window and the target
    // probe are.
    const bool target = g.opt.dnd_target_mode || (on && on == &g.second);
    const char* mime = target && g.opt.dnd_accept && offer_lists(g.opt.dnd_accept)
                           ? g.opt.dnd_accept
                           : nullptr;
    g.dnd_accepted = mime != nullptr;
    std::snprintf(g.dnd_accepted_mime, sizeof(g.dnd_accepted_mime), "%s", mime ? mime : "");
    wl_data_offer_accept(o, serial, mime);
    if (wl_data_offer_get_version(o) >= 3)
        wl_data_offer_set_actions(o, target ? g.opt.dnd_actions : 0u,
                                  target ? g.opt.dnd_preferred : 0u);
}
void on_dd_leave(void*, wl_data_device*) {
    std::printf("dnd leave\n");
    // After a drop the offer lives on until the data is read and finished.
    if (!g.dnd_dropped)
        dnd_forget_offer();
}
void on_dd_motion(void*, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    std::printf("dnd motion x=%.0f y=%.0f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
void on_dd_drop(void*, wl_data_device*) {
    std::printf("dnd drop\n");
    g.dnd_dropped = true;
    if (!g.dnd_offer || !g.dnd_accepted || g.recv_fd >= 0)
        return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    wl_data_offer_receive(g.dnd_offer, g.dnd_accepted_mime, fds[1]);
    close(fds[1]);
    g.recv_fd = fds[0];
    g.recv_len = 0;
    g.recv_label = "dnd";
}
void on_dd_selection(void*, wl_data_device*, wl_data_offer* o) {
    if (g.offer && g.offer != o)
        wl_data_offer_destroy(g.offer);
    g.offer = o;
    if (g.opt.primary || !g.opt.selection_mode)
        return;
    on_selection_offer(o != nullptr, [o](int fd) { wl_data_offer_receive(o, g.opt.receive, fd); });
}
const wl_data_device_listener data_device_listener = {on_dd_data_offer, on_dd_enter,
                                                      on_dd_leave, on_dd_motion, on_dd_drop,
                                                      on_dd_selection};

#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
void on_poffer_offer(void*, zwp_primary_selection_offer_v1*, const char* mime) {
    note_offer_mime(mime);
}
const zwp_primary_selection_offer_v1_listener poffer_listener = {on_poffer_offer};
void on_pd_data_offer(void*, zwp_primary_selection_device_v1*,
                      zwp_primary_selection_offer_v1* o) {
    g.offer_mimes[0] = '\0';
    g.offer_has_receive = false;
    zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, nullptr);
}
void on_pd_selection(void*, zwp_primary_selection_device_v1*,
                     zwp_primary_selection_offer_v1* o) {
    if (g.primary_offer && g.primary_offer != o)
        zwp_primary_selection_offer_v1_destroy(g.primary_offer);
    g.primary_offer = o;
    if (!g.opt.primary)
        return;
    on_selection_offer(o != nullptr, [o](int fd) {
        zwp_primary_selection_offer_v1_receive(o, g.opt.receive, fd);
    });
}
const zwp_primary_selection_device_v1_listener primary_device_listener = {on_pd_data_offer,
                                                                          on_pd_selection};
#endif

/// Drains the pipe of a --receive; prints the data at EOF.
void pump_receive() {
    if (g.recv_fd < 0)
        return;
    for (;;) {
        const ssize_t n = read(g.recv_fd, g.recv_buf + g.recv_len,
                               sizeof(g.recv_buf) - 1 - g.recv_len);
        if (n > 0) {
            g.recv_len += static_cast<size_t>(n);
            if (g.recv_len < sizeof(g.recv_buf) - 1)
                continue;
        } else if (n < 0 && errno == EAGAIN) {
            return;
        }
        g.recv_buf[g.recv_len] = '\0';
        close(g.recv_fd);
        g.recv_fd = -1;
        if (std::strcmp(g.recv_label, "dnd") == 0) {
            std::printf("dnd received data=\"%s\"\n", g.recv_buf);
            g.recv_label = "selection";
            if (g.dnd_offer && !g.opt.no_finish) {
                if (wl_data_offer_get_version(g.dnd_offer) >= 3) {
                    wl_data_offer_finish(g.dnd_offer);
                    std::printf("dnd finish\n");
                }
                dnd_forget_offer();
            }
            return;
        }
        std::printf("selection received mime=%s data=\"%s\"\n", g.opt.receive, g.recv_buf);
        return;
    }
}

void create_selection_devices() {
    if (!(g.opt.selection_mode || g.opt.dnd_source_mode || g.opt.dnd_target_mode) || !g.seat)
        return;
    if (g.ddm && !g.data_device) {
        g.data_device = wl_data_device_manager_get_data_device(g.ddm, g.seat);
        wl_data_device_add_listener(g.data_device, &data_device_listener, nullptr);
    }
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    if (g.psm && !g.primary_device) {
        g.primary_device = zwp_primary_selection_device_manager_v1_get_device(g.psm, g.seat);
        zwp_primary_selection_device_v1_add_listener(g.primary_device,
                                                     &primary_device_listener, nullptr);
    }
#endif
}

// ── drag-and-drop: source side ──

void on_dsrc_target(void*, wl_data_source*, const char* mime) {
    std::printf("dnd source target mime=%s\n", mime ? mime : "nil");
}
void on_dsrc_send(void*, wl_data_source*, const char* mime, int32_t fd) {
    std::printf("dnd source send mime=%s\n", mime);
    (void)!write(fd, g.opt.data, std::strlen(g.opt.data));
    close(fd);
}
void on_dsrc_cancelled(void*, wl_data_source* src) {
    std::printf("dnd source cancelled\n");
    wl_data_source_destroy(src);
    if (g.dnd_source == src)
        g.dnd_source = nullptr;
}
void on_dsrc_drop(void*, wl_data_source*) { std::printf("dnd source drop_performed\n"); }
void on_dsrc_finished(void*, wl_data_source* src) {
    std::printf("dnd source finished\n");
    wl_data_source_destroy(src);
    if (g.dnd_source == src)
        g.dnd_source = nullptr;
}
void on_dsrc_action(void*, wl_data_source*, uint32_t a) {
    std::printf("dnd source action=%u\n", a);
}
const wl_data_source_listener dnd_source_listener = {on_dsrc_target,    on_dsrc_send,
                                                     on_dsrc_cancelled, on_dsrc_drop,
                                                     on_dsrc_finished,  on_dsrc_action};

/// start_drag from the toplevel, quoting the press serial (or the enter's: --stale-serial).
void start_drag(uint32_t press_serial) {
    if (g.drag_started || !g.data_device)
        return;
    g.drag_started = true;
    const uint32_t serial = g.opt.stale_serial ? g.enter_serial : press_serial;
    if (!g.opt.null_source) {
        g.dnd_source = wl_data_device_manager_create_data_source(g.ddm);
        wl_data_source_add_listener(g.dnd_source, &dnd_source_listener, nullptr);
        char list[512];
        std::snprintf(list, sizeof(list), "%s", g.opt.mimes);
        for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
            wl_data_source_offer(g.dnd_source, t);
        if (wl_data_source_get_version(g.dnd_source) >= 3)
            wl_data_source_set_actions(g.dnd_source, g.opt.dnd_actions);
    }
    if (g.opt.icon_w > 0 && g.opt.icon_h > 0)
        g.icon = wl_compositor_create_surface(g.compositor);
    wl_data_device_start_drag(g.data_device, g.dnd_source, g.top.surface, g.icon, serial);
    if (g.icon) {
        // After start_drag, as GTK does: the role is set, the content follows. The attach
        // offset puts the hotspot at the icon's centre.
        g.icon_buffer = make_buffer(g.opt.icon_w, g.opt.icon_h, 0xffc04040);
        wl_surface_attach(g.icon, g.icon_buffer, -g.opt.icon_w / 2, -g.opt.icon_h / 2);
        wl_surface_damage_buffer(g.icon, 0, 0, g.opt.icon_w, g.opt.icon_h);
        wl_surface_commit(g.icon);
        std::printf("dnd icon w=%d h=%d dx=%d dy=%d\n", g.opt.icon_w, g.opt.icon_h,
                    -g.opt.icon_w / 2, -g.opt.icon_h / 2);
    }
    std::printf("dnd source start serial=%u\n", serial);
    if (g.opt.exit_during_drag >= 0.0)
        g.exit_at = now_s() + g.opt.exit_during_drag;
}

unsigned action_bits(const char* list) {
    unsigned bits = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", list);
    for (char* t = std::strtok(buf, ","); t; t = std::strtok(nullptr, ",")) {
        if (!std::strcmp(t, "copy")) bits |= 1;
        else if (!std::strcmp(t, "move")) bits |= 2;
        else if (!std::strcmp(t, "ask")) bits |= 4;
        else if (!std::strcmp(t, "none")) bits |= 0;
        else bits |= static_cast<unsigned>(std::atoi(t));
    }
    return bits;
}

// ── xdg-activation ──

#if defined(LUMEN_PROBE_HAS_ACTIVATION)
void on_token_done(void*, xdg_activation_token_v1* t, const char* token) {
    xdg_activation_token_v1_destroy(t);
    if (g.opt.token_file) {
        char tmp[512];
        std::snprintf(tmp, sizeof(tmp), "%s.tmp", g.opt.token_file);
        if (FILE* f = std::fopen(tmp, "w")) {
            std::fputs(token, f);
            std::fclose(f);
            std::rename(tmp, g.opt.token_file);
        }
    }
    std::printf("activation token=%.8s\n", token);
}
const xdg_activation_token_v1_listener token_listener = {on_token_done};
#endif

/// The issuer: a token for the press with `serial`.
void request_token(uint32_t serial) {
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    if (!g.activation)
        return;
    xdg_activation_token_v1* t = xdg_activation_v1_get_activation_token(g.activation);
    xdg_activation_token_v1_add_listener(t, &token_listener, nullptr);
    if (!g.opt.no_serial)
        xdg_activation_token_v1_set_serial(t, serial, g.seat);
    xdg_activation_token_v1_set_surface(t, g.top.surface);
    xdg_activation_token_v1_set_app_id(t, "lumen-probe");
    xdg_activation_token_v1_commit(t);
    if (g.opt.no_serial)
        std::printf("activation request serial=none\n");
    else
        std::printf("activation request serial=%u\n", serial);
#else
    (void)serial;
#endif
}

/// The target: presents its token once it has one (--token, or the file once it exists).
void try_activate(bool mapped) {
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    if (g.activated || !g.activation || !g.opt.activation_target)
        return;
    char buf[256]{};
    const char* token = g.opt.token;
    if (!token && g.opt.token_file) {
        FILE* f = std::fopen(g.opt.token_file, "r");
        if (!f)
            return;
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (!buf[0])
            return;
        token = buf;
    }
    if (!token)
        return;
    g.activated = true;
    xdg_activation_v1_activate(g.activation, token, g.top.surface);
    std::printf("activation activate token=%.8s mapped=%d\n", token, mapped ? 1 : 0);
#else
    (void)mapped;
#endif
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
    if (g.opt.selection_mode) {
        if (g.opt.trigger == options::set_on::press)
            set_selection(serial);
        return;
    }
    if (g.opt.dnd_source_mode) {
        if (g.pointer_on == &g.top)
            start_drag(serial);
        return;
    }
    if (g.opt.dnd_target_mode)
        return;
    if (g.opt.activation_issuer || g.opt.activation_target) {
        if (g.opt.activation_issuer && g.pointer_on == &g.top)
            request_token(serial);
        return;
    }
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
void on_key_enter(void*, wl_keyboard*, uint32_t serial, wl_surface* surface, wl_array*) {
    char buf[16];
    std::printf("keyboard enter surface=%s\n", name_of(find(surface), buf, sizeof(buf)));
    if (g.opt.selection_mode && g.opt.trigger == options::set_on::focus)
        set_selection(serial);
}
void on_key_leave(void*, wl_keyboard*, uint32_t, wl_surface*) { std::printf("keyboard leave\n"); }
void on_key(void*, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
    std::printf("key code=%u state=%u\n", key, state);
    if (g.opt.selection_mode && g.opt.trigger == options::set_on::key &&
        state == WL_KEYBOARD_KEY_STATE_PRESSED)
        set_selection(serial);
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
    else if (std::strcmp(iface, wl_data_device_manager_interface.name) == 0)
        g.ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(r, name, &wl_data_device_manager_interface, version < 3 ? version : 3));
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    else if (std::strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name) == 0)
        g.psm = static_cast<zwp_primary_selection_device_manager_v1*>(
            wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1));
#endif
    else if (std::strcmp(iface, wl_output_interface.name) == 0 && !g.output_global) {
        g.output_global = name;
        g.output_version = version;
    }
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
    else if (std::strcmp(iface, zxdg_output_manager_v1_interface.name) == 0)
        g.xom = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, version < 3 ? version : 3));
#endif
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    else if (std::strcmp(iface, xdg_activation_v1_interface.name) == 0)
        g.activation = static_cast<xdg_activation_v1*>(
            wl_registry_bind(r, name, &xdg_activation_v1_interface, 1));
#endif
#if defined(LUMEN_PROBE_HAS_DECORATION)
    else if (std::strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0)
        g.deco_manager = static_cast<zxdg_decoration_manager_v1*>(
            wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1));
#endif
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
                 "         [--exit-after S]\n"
                 "       lumen-probe selection [--kind clipboard|primary] [--mimes a,b]\n"
                 "         [--data STR] [--set-on press|key|focus|never] [--stale-serial]\n"
                 "         [--reuse-source] [--receive MIME] [--exit-after S]\n"
                 "       lumen-probe dnd-source [--mimes a,b] [--data STR] [--actions LIST]\n"
                 "         [--icon WxH] [--null-source] [--stale-serial] [--second-window]\n"
                 "         [--exit-during-drag S] [--exit-after S]\n"
                 "       lumen-probe dnd-target [--accept MIME|none] [--actions LIST]\n"
                 "         [--preferred A] [--no-finish] [--exit-after S]\n"
                 "       lumen-probe outputs [--late S] [--unmap-after S] [popup options]\n"
                 "       lumen-probe activation-issuer --token-file F [--no-serial]\n"
                 "       lumen-probe activation-target (--token STR | --token-file F) [--early]\n"
                 "       any toplevel mode: [--ignore-ping-for S] [--freeze S] [--ssd]\n"
                 "         [--title T]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2 || (std::strcmp(argv[1], "popup") != 0 && std::strcmp(argv[1], "layer") != 0 &&
                     std::strcmp(argv[1], "selection") != 0 &&
                     std::strcmp(argv[1], "dnd-source") != 0 &&
                     std::strcmp(argv[1], "dnd-target") != 0 &&
                     std::strcmp(argv[1], "outputs") != 0 &&
                     std::strcmp(argv[1], "activation-issuer") != 0 &&
                     std::strcmp(argv[1], "activation-target") != 0))
        return usage();
    options& o = g.opt;
    o.layer_mode = std::strcmp(argv[1], "layer") == 0;
    o.selection_mode = std::strcmp(argv[1], "selection") == 0;
    o.dnd_source_mode = std::strcmp(argv[1], "dnd-source") == 0;
    o.dnd_target_mode = std::strcmp(argv[1], "dnd-target") == 0;
    o.outputs_mode = std::strcmp(argv[1], "outputs") == 0;
    o.activation_issuer = std::strcmp(argv[1], "activation-issuer") == 0;
    o.activation_target = std::strcmp(argv[1], "activation-target") == 0;
    if (o.dnd_source_mode || o.dnd_target_mode)
        o.mimes = "text/plain;charset=utf-8,text/plain";
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
        else if (!std::strcmp(a, "--kind") && v) { o.primary = !std::strcmp(v, "primary"); ++i; }
        else if (!std::strcmp(a, "--mimes") && v) { o.mimes = v; ++i; }
        else if (!std::strcmp(a, "--data") && v) { o.data = v; ++i; }
        else if (!std::strcmp(a, "--set-on") && v) {
            o.trigger = !std::strcmp(v, "key")     ? options::set_on::key
                        : !std::strcmp(v, "focus") ? options::set_on::focus
                        : !std::strcmp(v, "never") ? options::set_on::never
                                                   : options::set_on::press;
            ++i;
        }
        else if (!std::strcmp(a, "--stale-serial")) o.stale_serial = true;
        else if (!std::strcmp(a, "--reuse-source")) o.reuse_source = true;
        else if (!std::strcmp(a, "--receive") && v) { o.receive = v; ++i; }
        else if (!std::strcmp(a, "--actions") && v) { o.dnd_actions = action_bits(v); ++i; }
        else if (!std::strcmp(a, "--preferred") && v) { o.dnd_preferred = action_bits(v); ++i; }
        else if (!std::strcmp(a, "--accept") && v) {
            o.dnd_accept = !std::strcmp(v, "none") ? nullptr : v;
            ++i;
        }
        else if (!std::strcmp(a, "--no-finish")) o.no_finish = true;
        else if (!std::strcmp(a, "--icon") && v && parse_pair(v, o.icon_w, o.icon_h, 'x')) ++i;
        else if (!std::strcmp(a, "--null-source")) o.null_source = true;
        else if (!std::strcmp(a, "--second-window")) o.second_window = true;
        else if (!std::strcmp(a, "--exit-during-drag") && v) { o.exit_during_drag = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--late") && v) { o.late_output = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--ignore-ping-for") && v) { o.ignore_ping_for = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--freeze") && v) { o.freeze = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--ssd")) o.ssd = true;
        else if (!std::strcmp(a, "--title") && v) { o.title = v; ++i; }
        else if (!std::strcmp(a, "--unmap-after") && v) { o.unmap_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--token-file") && v) { o.token_file = v; ++i; }
        else if (!std::strcmp(a, "--token") && v) { o.token = v; ++i; }
        else if (!std::strcmp(a, "--no-serial")) o.no_serial = true;
        else if (!std::strcmp(a, "--early")) o.early = true;
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
    g.registry = registry;
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(g.display);
    if (!g.compositor || !g.shm || !g.wm_base || !g.seat) {
        std::fprintf(stderr, "lumen-probe: missing a required global\n");
        return 1;
    }
    xdg_wm_base_add_listener(g.wm_base, &wm_base_listener, nullptr);
    wl_seat_add_listener(g.seat, &seat_listener, nullptr);
    // Before the window exists: the devices must be there when the keyboard enter comes.
    create_selection_devices();

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
    if (o.outputs_mode && o.late_output <= 0.0)
        bind_outputs();
    if (!o.layer_mode) {
        g.top.surface = wl_compositor_create_surface(g.compositor);
        if (o.outputs_mode)
            wl_surface_add_listener(g.top.surface, &surface_listener, nullptr);
        g.top.xdg = xdg_wm_base_get_xdg_surface(g.wm_base, g.top.surface);
        xdg_surface_add_listener(g.top.xdg, &xdg_listener, &g.top);
        g.top.toplevel = xdg_surface_get_toplevel(g.top.xdg);
        xdg_toplevel_add_listener(g.top.toplevel, &toplevel_listener, &g.top);
        xdg_toplevel_set_title(g.top.toplevel, o.title);
        xdg_toplevel_set_app_id(g.top.toplevel, "lumen-probe");
#if defined(LUMEN_PROBE_HAS_DECORATION)
        if (o.ssd && g.deco_manager) {
            // Before the initial commit: the first configure then carries the mode.
            g.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(g.deco_manager,
                                                                        g.top.toplevel);
            zxdg_toplevel_decoration_v1_add_listener(g.deco, &deco_listener, nullptr);
            zxdg_toplevel_decoration_v1_set_mode(g.deco,
                                                 ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
#endif
        if (o.activation_target && o.early)
            try_activate(false); // before the initial commit: not mapped yet
        wl_surface_commit(g.top.surface);
        if (o.second_window) {
            // A second toplevel of this client: a drop target inside the source's own client.
            g.second.index = -2;
            g.second.colour = 0xff50a060;
            g.second.surface = wl_compositor_create_surface(g.compositor);
            g.second.xdg = xdg_wm_base_get_xdg_surface(g.wm_base, g.second.surface);
            xdg_surface_add_listener(g.second.xdg, &xdg_listener, &g.second);
            g.second.toplevel = xdg_surface_get_toplevel(g.second.xdg);
            xdg_toplevel_add_listener(g.second.toplevel, &toplevel_listener, &g.second);
            xdg_toplevel_set_title(g.second.toplevel, "lumen-probe-2");
            xdg_toplevel_set_app_id(g.second.toplevel, "lumen-probe-2");
            wl_surface_commit(g.second.surface);
        }
    }

    const double deadline = now_s() + o.exit_after;
    int rc = 0;
    while (!g_stop && !g.closed && now_s() < deadline) {
        if (g.exit_at > 0.0 && now_s() >= g.exit_at) {
            // Client death mid-drag: no cleanup, no goodbye — the socket just closes.
            std::printf("dnd source exiting\n");
            std::fflush(stdout);
            _exit(0);
        }
        if (g.unmap_at > 0.0 && now_s() >= g.unmap_at && !g.unmapped) {
            // Null buffer: the surface unmaps and goes back to before its initial commit;
            // this probe stays unmapped until it exits.
            g.unmapped = true;
            wl_surface_attach(g.top.surface, nullptr, 0, 0);
            wl_surface_commit(g.top.surface);
            std::printf("%s unmapped\n", o.layer_mode ? "layer" : "toplevel");
        }
        if (o.activation_target && !o.early && g.top.configured)
            try_activate(true);
        if (g.bind_output_at > 0.0 && now_s() >= g.bind_output_at) {
            g.bind_output_at = 0.0;
            bind_outputs();
        }
        if (g.deferred_ping && g.ignore_until > 0.0 && now_s() >= g.ignore_until) {
            std::printf("pong serial=%u late\n", g.deferred_ping);
            xdg_wm_base_pong(g.wm_base, g.deferred_ping);
            g.deferred_ping = 0;
        }
        if (g.freeze_at > 0.0 && now_s() >= g.freeze_at) {
            // A hung client: nothing is read or dispatched; the compositor's events queue up
            // in the socket (and its connection buffer) meanwhile.
            g.freeze_at = 0.0;
            wl_display_flush(g.display);
            std::printf("freeze s=%g\n", o.freeze);
            std::fflush(stdout);
            const double until = now_s() + o.freeze;
            while (!g_stop && now_s() < until)
                usleep(20000);
            std::printf("thaw\n");
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
        const int ready = poll(&pfd, 1, g.recv_fd >= 0 ? 10 : 50);
        pump_receive();
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
