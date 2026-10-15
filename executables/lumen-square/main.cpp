// Interactive move/resize demo client: one plain coloured square.
//
// Talks libwayland-client directly, like lumen-clouds. The window is a solid fill with a
// darker 12 px border band; a left-button press in the band starts xdg_toplevel.resize with
// the matching edges, anywhere else starts xdg_toplevel.move. It hovers the band with the
// matching resize cursor through wp_cursor_shape_v1 when the compositor offers it.
//
// Every protocol event of interest is printed as one stdout line so a smoke test can grep
// the sequence:
//
//   configure serial=<n> size=<w>x<h> states=<comma list|none>
//   commit size=<w>x<h>
//   pointer enter x=<x> y=<y>        pointer leave
//   press button=<code> x=<x> y=<y> -> move | -> resize edges=<n> | -> none
//   release button=<code>
//   cursor shape=<name>
//
//   lumen-square [-t seconds]
//
// Behaviour that matters for a frame-perfect compositor:
//   - A configure is acked in the same dispatch as the commit of a buffer of exactly the
//     configured size (clamped to the advertised minimum). If both buffers are still held,
//     the ack waits for a wl_buffer.release instead of committing a mismatched frame; a
//     newer configure supersedes a waiting one, so only the latest serial is ever acked.
//   - Pixels are repainted only when the buffer size changes. Each of the two buffers lives
//     in its own memfd-backed wl_shm_pool that grows geometrically, so an interactive
//     resize creates a new wl_buffer per size but not a new fd, mapping or pool.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_SQUARE_HAS_CURSOR_SHAPE)
#include "cursor-shape-v1-client-protocol.h"
#endif

namespace {

constexpr int k_initial_width = 400;
constexpr int k_initial_height = 300;
constexpr int k_min_width = 120;
constexpr int k_min_height = 90;
constexpr int k_border = 12; // resize band, in surface pixels
constexpr int k_buffer_count = 2;

constexpr uint32_t k_fill_colour = 0xff3d7bd9;   // XRGB: mid blue
constexpr uint32_t k_border_colour = 0xff1c3a6b; // same hue, much darker

// xdg_toplevel.resize_edge values form a bitmask: top=1, bottom=2, left=4, right=8.
constexpr uint32_t k_edge_top = 1;
constexpr uint32_t k_edge_bottom = 2;
constexpr uint32_t k_edge_left = 4;
constexpr uint32_t k_edge_right = 8;

volatile sig_atomic_t g_running = 1;

void on_signal(int) { g_running = 0; }

struct shm_slot {
    int fd = -1;
    void* map = nullptr;
    size_t capacity = 0;
    wl_shm_pool* pool = nullptr;
    wl_buffer* buffer = nullptr;
    int width = 0;
    int height = 0;
    bool busy = false;
};

struct app {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
#if defined(LUMEN_SQUARE_HAS_CURSOR_SHAPE)
    wp_cursor_shape_manager_v1* cursor_manager = nullptr;
    wp_cursor_shape_device_v1* cursor_device = nullptr;
#endif

    wl_surface* surface = nullptr;
    xdg_surface* xdg_surf = nullptr;
    xdg_toplevel* toplevel = nullptr;

    shm_slot slots[k_buffer_count]{};

    // Committed state.
    int width = k_initial_width;
    int height = k_initial_height;
    bool committed_once = false;
    bool sized_by_state = false; // last commit was maximized/fullscreen/tiled
    int floating_width = k_initial_width;
    int floating_height = k_initial_height;

    // Pending configure: xdg_toplevel.configure accumulates, xdg_surface.configure latches.
    int next_width = 0;
    int next_height = 0;
    bool next_sized_by_state = false;
    int pending_width = 0;
    int pending_height = 0;
    bool pending_sized_by_state = false;
    uint32_t pending_serial = 0;
    bool configure_pending = false;

    // Pointer.
    uint32_t enter_serial = 0;
    bool pointer_inside = false;
    double pointer_x = 0.0;
    double pointer_y = 0.0;
    uint32_t hover_edges = ~0u; // forces the first cursor update after enter
};

double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

uint32_t edges_at(const app& a, double x, double y) {
    uint32_t edges = 0;
    if (y < k_border)
        edges |= k_edge_top;
    else if (y >= a.height - k_border)
        edges |= k_edge_bottom;
    if (x < k_border)
        edges |= k_edge_left;
    else if (x >= a.width - k_border)
        edges |= k_edge_right;
    return edges;
}

// ── painting ────────────────────────────────────────────────────────────────

void paint(uint32_t* pixels, int width, int height) {
    const auto w = static_cast<size_t>(width);
    const int band_x = std::min(k_border, (width + 1) / 2);
    const int band_y = std::min(k_border, (height + 1) / 2);

    // One border row and one interior row, then copies: the fill is memory-bound anyway.
    uint32_t* border_row = pixels;
    std::fill_n(border_row, w, k_border_colour);
    for (int y = 1; y < band_y; ++y)
        std::memcpy(pixels + static_cast<size_t>(y) * w, border_row, w * sizeof(uint32_t));

    if (band_y < height - band_y) {
        uint32_t* mid_row = pixels + static_cast<size_t>(band_y) * w;
        std::fill_n(mid_row, w, k_fill_colour);
        std::fill_n(mid_row, static_cast<size_t>(band_x), k_border_colour);
        std::fill_n(mid_row + (w - static_cast<size_t>(band_x)), static_cast<size_t>(band_x),
                    k_border_colour);
        for (int y = band_y + 1; y < height - band_y; ++y)
            std::memcpy(pixels + static_cast<size_t>(y) * w, mid_row, w * sizeof(uint32_t));
    }

    for (int y = std::max(band_y, height - band_y); y < height; ++y)
        std::memcpy(pixels + static_cast<size_t>(y) * w, border_row, w * sizeof(uint32_t));
}

// ── shm buffers ─────────────────────────────────────────────────────────────

void buffer_release(void* data, wl_buffer*);
const wl_buffer_listener buffer_listener = {buffer_release};

void destroy_slot(shm_slot& s) {
    if (s.buffer)
        wl_buffer_destroy(s.buffer);
    if (s.pool)
        wl_shm_pool_destroy(s.pool);
    if (s.map)
        munmap(s.map, s.capacity);
    if (s.fd >= 0)
        close(s.fd);
    s = shm_slot{};
}

// Makes `s` hold a wl_buffer of exactly width x height with the square painted into it.
// Returns false on allocation failure.
bool prepare_slot(app& a, shm_slot& s, int width, int height) {
    if (s.buffer && s.width == width && s.height == height)
        return true; // pixels depend only on size: nothing to repaint

    const size_t stride = static_cast<size_t>(width) * 4;
    const size_t needed = stride * static_cast<size_t>(height);
    if (needed > static_cast<size_t>(INT32_MAX)) {
        std::fprintf(stderr, "lumen-square: %dx%d is too large for wl_shm\n", width, height);
        return false;
    }

    if (needed > s.capacity) {
        // Grow by at least half again so a drag-resize does not resize the pool every step.
        constexpr size_t page = 4096;
        size_t capacity = std::max(needed, s.capacity + s.capacity / 2);
        capacity = std::min((capacity + page - 1) & ~(page - 1), static_cast<size_t>(INT32_MAX));

        if (s.fd < 0) {
            s.fd = memfd_create("lumen-square", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (s.fd < 0) {
                std::fprintf(stderr, "lumen-square: memfd_create: %s\n", std::strerror(errno));
                return false;
            }
        }
        if (ftruncate(s.fd, static_cast<off_t>(capacity)) < 0) {
            std::fprintf(stderr, "lumen-square: ftruncate: %s\n", std::strerror(errno));
            return false;
        }
        // The compositor maps this file; forbidding shrink rules out SIGBUS on its side.
        (void)fcntl(s.fd, F_ADD_SEALS, F_SEAL_SHRINK);

        void* map = s.map ? mremap(s.map, s.capacity, capacity, MREMAP_MAYMOVE)
                          : mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0);
        if (map == MAP_FAILED) {
            std::fprintf(stderr, "lumen-square: mmap: %s\n", std::strerror(errno));
            return false;
        }
        s.map = map;
        s.capacity = capacity;

        if (s.pool)
            wl_shm_pool_resize(s.pool, static_cast<int32_t>(capacity));
        else
            s.pool = wl_shm_create_pool(a.shm, s.fd, static_cast<int32_t>(capacity));
    }

    if (s.buffer)
        wl_buffer_destroy(s.buffer); // only called on a released slot
    s.buffer = wl_shm_pool_create_buffer(s.pool, 0, width, height, static_cast<int32_t>(stride),
                                         WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(s.buffer, &buffer_listener, &a);
    s.width = width;
    s.height = height;
    paint(static_cast<uint32_t*>(s.map), width, height);
    return true;
}

// ── configure → ack → commit ────────────────────────────────────────────────

void apply_configure(app& a) {
    if (!a.configure_pending)
        return;

    int width = a.pending_width;
    int height = a.pending_height;
    if (width <= 0)
        width = (a.sized_by_state && !a.pending_sized_by_state) ? a.floating_width : a.width;
    if (height <= 0)
        height = (a.sized_by_state && !a.pending_sized_by_state) ? a.floating_height : a.height;
    // A compositor may go below the advertised minimum; the client keeps its own floor and
    // the configure/commit lines show the difference.
    width = std::max(width, k_min_width);
    height = std::max(height, k_min_height);

    if (a.committed_once && width == a.width && height == a.height) {
        // State-only change (e.g. activated): the attached buffer already matches.
        xdg_surface_ack_configure(a.xdg_surf, a.pending_serial);
        wl_surface_commit(a.surface);
    } else {
        shm_slot* slot = nullptr;
        for (auto& s : a.slots) {
            if (!s.busy) {
                slot = &s;
                break;
            }
        }
        if (!slot)
            return; // both buffers held: ack together with the commit once one is released

        if (!prepare_slot(a, *slot, width, height)) {
            g_running = 0;
            return;
        }
        xdg_surface_ack_configure(a.xdg_surf, a.pending_serial);
        wl_surface_attach(a.surface, slot->buffer, 0, 0);
        wl_surface_damage_buffer(a.surface, 0, 0, width, height);
        wl_surface_commit(a.surface);
        slot->busy = true;
    }

    a.configure_pending = false;
    a.committed_once = true;
    a.width = width;
    a.height = height;
    a.sized_by_state = a.pending_sized_by_state;
    if (!a.sized_by_state) {
        a.floating_width = width;
        a.floating_height = height;
    }
    std::printf("commit size=%dx%d\n", width, height);
}

void buffer_release(void* data, wl_buffer* buffer) {
    auto& a = *static_cast<app*>(data);
    for (auto& s : a.slots) {
        if (s.buffer == buffer)
            s.busy = false;
    }
    apply_configure(a);
}

const char* state_name(uint32_t state) {
    switch (state) {
    case 1: return "maximized";
    case 2: return "fullscreen";
    case 3: return "resizing";
    case 4: return "activated";
    case 5: return "tiled_left";
    case 6: return "tiled_right";
    case 7: return "tiled_top";
    case 8: return "tiled_bottom";
    case 9: return "suspended";
    case 10: return "constrained_left";
    case 11: return "constrained_right";
    case 12: return "constrained_top";
    case 13: return "constrained_bottom";
    default: return nullptr;
    }
}

char g_states_text[256] = "none";

void toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                        wl_array* states) {
    auto& a = *static_cast<app*>(data);
    a.next_width = width;
    a.next_height = height;
    a.next_sized_by_state = false;

    size_t len = 0;
    g_states_text[0] = '\0';
    const auto* it = static_cast<const uint32_t*>(states->data);
    const size_t count = states->size / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t s = it[i];
        if (s == XDG_TOPLEVEL_STATE_MAXIMIZED || s == XDG_TOPLEVEL_STATE_FULLSCREEN ||
            (s >= XDG_TOPLEVEL_STATE_TILED_LEFT && s <= XDG_TOPLEVEL_STATE_TILED_BOTTOM))
            a.next_sized_by_state = true;

        const char* name = state_name(s);
        char unknown[16];
        if (!name) {
            std::snprintf(unknown, sizeof unknown, "%u", s);
            name = unknown;
        }
        const int n = std::snprintf(g_states_text + len, sizeof g_states_text - len, "%s%s",
                                    len ? "," : "", name);
        if (n < 0 || static_cast<size_t>(n) >= sizeof g_states_text - len)
            break;
        len += static_cast<size_t>(n);
    }
    if (len == 0)
        std::strcpy(g_states_text, "none");
}

void toplevel_close(void*, xdg_toplevel*) { g_running = 0; }

void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}

void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

const xdg_toplevel_listener toplevel_listener = {
    toplevel_configure,
    toplevel_close,
    toplevel_configure_bounds,
    toplevel_wm_capabilities,
};

void xdg_surface_configure(void* data, xdg_surface*, uint32_t serial) {
    auto& a = *static_cast<app*>(data);
    std::printf("configure serial=%u size=%dx%d states=%s\n", serial, a.next_width,
                a.next_height, g_states_text);
    // A newer configure supersedes one still waiting for a free buffer; acking the latest
    // serial implicitly acks the earlier ones.
    a.pending_serial = serial;
    a.pending_width = a.next_width;
    a.pending_height = a.next_height;
    a.pending_sized_by_state = a.next_sized_by_state;
    a.configure_pending = true;
    apply_configure(a);
}
const xdg_surface_listener xdg_surface_listener_impl = {xdg_surface_configure};

void wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener wm_base_listener = {wm_base_ping};

// ── pointer ─────────────────────────────────────────────────────────────────

void update_cursor(app& a) {
#if defined(LUMEN_SQUARE_HAS_CURSOR_SHAPE)
    if (!a.cursor_device || !a.pointer_inside)
        return;
    const uint32_t edges = edges_at(a, a.pointer_x, a.pointer_y);
    if (edges == a.hover_edges)
        return;
    a.hover_edges = edges;

    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    const char* name = "default";
    switch (edges) {
    case k_edge_top: shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE; name = "n_resize"; break;
    case k_edge_bottom: shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE; name = "s_resize"; break;
    case k_edge_left: shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE; name = "w_resize"; break;
    case k_edge_right: shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE; name = "e_resize"; break;
    case k_edge_top | k_edge_left:
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
        name = "nw_resize";
        break;
    case k_edge_top | k_edge_right:
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
        name = "ne_resize";
        break;
    case k_edge_bottom | k_edge_left:
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
        name = "sw_resize";
        break;
    case k_edge_bottom | k_edge_right:
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
        name = "se_resize";
        break;
    default: break;
    }
    wp_cursor_shape_device_v1_set_shape(a.cursor_device, a.enter_serial, shape);
    std::printf("cursor shape=%s\n", name);
#else
    (void)a;
#endif
}

void pointer_enter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t x,
                   wl_fixed_t y) {
    auto& a = *static_cast<app*>(data);
    if (surface != a.surface)
        return;
    a.enter_serial = serial;
    a.pointer_inside = true;
    a.pointer_x = wl_fixed_to_double(x);
    a.pointer_y = wl_fixed_to_double(y);
    a.hover_edges = ~0u;
    std::printf("pointer enter x=%.1f y=%.1f\n", a.pointer_x, a.pointer_y);
    update_cursor(a);
}

void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface* surface) {
    auto& a = *static_cast<app*>(data);
    if (surface != a.surface)
        return;
    a.pointer_inside = false;
    std::printf("pointer leave\n");
}

void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto& a = *static_cast<app*>(data);
    a.pointer_x = wl_fixed_to_double(x);
    a.pointer_y = wl_fixed_to_double(y);
    update_cursor(a);
}

void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button,
                    uint32_t state) {
    auto& a = *static_cast<app*>(data);
    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        std::printf("release button=%u\n", button);
        return;
    }

    std::printf("press button=%u x=%.1f y=%.1f ", button, a.pointer_x, a.pointer_y);
    if (button != BTN_LEFT || !a.pointer_inside) {
        std::printf("-> none\n");
        return;
    }
    const uint32_t edges = edges_at(a, a.pointer_x, a.pointer_y);
    if (edges != 0) {
        std::printf("-> resize edges=%u\n", edges);
        xdg_toplevel_resize(a.toplevel, a.seat, serial, edges);
    } else {
        std::printf("-> move\n");
        xdg_toplevel_move(a.toplevel, a.seat, serial);
    }
}

void pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void pointer_frame(void*, wl_pointer*) {}
void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

// wl_seat is bound at version 5, so no later wl_pointer event can arrive. Value-initialising
// first keeps the later members (which vary with the libwayland version) null without naming
// them.
constexpr wl_pointer_listener make_pointer_listener() {
    wl_pointer_listener l{};
    l.enter = pointer_enter;
    l.leave = pointer_leave;
    l.motion = pointer_motion;
    l.button = pointer_button;
    l.axis = pointer_axis;
    l.frame = pointer_frame;
    l.axis_source = pointer_axis_source;
    l.axis_stop = pointer_axis_stop;
    l.axis_discrete = pointer_axis_discrete;
    return l;
}
const wl_pointer_listener pointer_listener = make_pointer_listener();

void attach_cursor_device(app& a) {
#if defined(LUMEN_SQUARE_HAS_CURSOR_SHAPE)
    if (a.cursor_manager && a.pointer && !a.cursor_device)
        a.cursor_device = wp_cursor_shape_manager_v1_get_pointer(a.cursor_manager, a.pointer);
#else
    (void)a;
#endif
}

void release_pointer(app& a) {
#if defined(LUMEN_SQUARE_HAS_CURSOR_SHAPE)
    if (a.cursor_device) {
        wp_cursor_shape_device_v1_destroy(a.cursor_device);
        a.cursor_device = nullptr;
    }
#endif
    if (a.pointer) {
        if (wl_pointer_get_version(a.pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
            wl_pointer_release(a.pointer);
        else
            wl_pointer_destroy(a.pointer);
        a.pointer = nullptr;
    }
    a.pointer_inside = false;
}

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto& a = *static_cast<app*>(data);
    const bool has_pointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_pointer && !a.pointer) {
        a.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a.pointer, &pointer_listener, &a);
        attach_cursor_device(a);
    } else if (!has_pointer && a.pointer) {
        release_pointer(a);
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = {seat_capabilities, seat_name};

// ── registry ────────────────────────────────────────────────────────────────

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
    auto& a = *static_cast<app*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        a.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        a.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        a.wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 7u)));
        xdg_wm_base_add_listener(a.wm_base, &wm_base_listener, &a);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !a.seat) {
        a.seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(a.seat, &seat_listener, &a);
#if defined(LUMEN_SQUARE_HAS_CURSOR_SHAPE)
    } else if (std::strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        a.cursor_manager = static_cast<wp_cursor_shape_manager_v1*>(
            wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1));
        attach_cursor_device(a);
#endif
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {registry_global, registry_global_remove};

void destroy_app(app& a) {
    for (auto& s : a.slots)
        destroy_slot(s);
    release_pointer(a);
#if defined(LUMEN_SQUARE_HAS_CURSOR_SHAPE)
    if (a.cursor_manager)
        wp_cursor_shape_manager_v1_destroy(a.cursor_manager);
#endif
    if (a.toplevel)
        xdg_toplevel_destroy(a.toplevel);
    if (a.xdg_surf)
        xdg_surface_destroy(a.xdg_surf);
    if (a.surface)
        wl_surface_destroy(a.surface);
    if (a.seat) {
        if (wl_seat_get_version(a.seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(a.seat);
        else
            wl_seat_destroy(a.seat);
    }
    if (a.wm_base)
        xdg_wm_base_destroy(a.wm_base);
    if (a.compositor)
        wl_compositor_destroy(a.compositor);
    if (a.shm)
        wl_shm_destroy(a.shm);
    if (a.registry)
        wl_registry_destroy(a.registry);
    if (a.display)
        wl_display_disconnect(a.display);
}

} // namespace

int main(int argc, char* argv[]) {
    double duration = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            duration = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: lumen-square [-t seconds]\n");
            return 2;
        }
    }

    // Line-buffered so a test reading a pipe sees each event as it happens.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // The signals stay blocked except inside ppoll, so a signal that lands between the
    // running check and the wait cannot be lost.
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigset_t blocked;
    sigset_t wait_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, &wait_mask);
    sigdelset(&wait_mask, SIGINT);
    sigdelset(&wait_mask, SIGTERM);

    app a{};
    a.display = wl_display_connect(nullptr);
    if (!a.display) {
        std::fprintf(stderr, "lumen-square: cannot connect to a Wayland display\n");
        return 1;
    }

    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    wl_display_roundtrip(a.display);

    if (!a.compositor || !a.shm || !a.wm_base) {
        std::fprintf(stderr, "lumen-square: missing wl_compositor, wl_shm or xdg_wm_base\n");
        destroy_app(a);
        return 1;
    }
    if (!a.seat)
        std::fprintf(stderr, "lumen-square: no wl_seat — the window cannot be dragged\n");

    a.surface = wl_compositor_create_surface(a.compositor);
    a.xdg_surf = xdg_wm_base_get_xdg_surface(a.wm_base, a.surface);
    xdg_surface_add_listener(a.xdg_surf, &xdg_surface_listener_impl, &a);
    a.toplevel = xdg_surface_get_toplevel(a.xdg_surf);
    xdg_toplevel_add_listener(a.toplevel, &toplevel_listener, &a);
    xdg_toplevel_set_title(a.toplevel, "lumen-square");
    xdg_toplevel_set_app_id(a.toplevel, "lumen-square");
    xdg_toplevel_set_min_size(a.toplevel, k_min_width, k_min_height);
    wl_surface_commit(a.surface); // initial commit, no buffer: asks for the first configure

    const double start = now_seconds();
    const int fd = wl_display_get_fd(a.display);
    int rc = 0;
    while (g_running) {
        while (wl_display_prepare_read(a.display) != 0) {
            if (wl_display_dispatch_pending(a.display) < 0) {
                rc = 1;
                g_running = 0;
                break;
            }
        }
        if (!g_running) {
            wl_display_cancel_read(a.display);
            break;
        }

        if (wl_display_flush(a.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(a.display);
            rc = 1;
            break;
        }

        timespec timeout{};
        timespec* timeout_ptr = nullptr;
        if (duration > 0.0) {
            const double left = std::max(0.0, duration - (now_seconds() - start));
            timeout.tv_sec = static_cast<time_t>(left);
            timeout.tv_nsec = static_cast<long>((left - static_cast<double>(timeout.tv_sec)) * 1e9);
            timeout_ptr = &timeout;
        }

        pollfd pfd{fd, POLLIN, 0};
        const int ready = ppoll(&pfd, 1, timeout_ptr, &wait_mask);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(a.display) < 0) {
                rc = 1;
                break;
            }
        } else {
            wl_display_cancel_read(a.display);
            if (ready < 0 && errno != EINTR) {
                rc = 1;
                break;
            }
            if (ready > 0) { // POLLHUP / POLLERR without data: the compositor went away
                rc = 1;
                break;
            }
        }

        if (wl_display_dispatch_pending(a.display) < 0) {
            rc = 1;
            break;
        }

        if (duration > 0.0 && now_seconds() - start >= duration)
            g_running = 0;
    }

    if (rc != 0)
        std::fprintf(stderr, "lumen-square: lost the Wayland connection\n");
    destroy_app(a);
    return rc;
}
