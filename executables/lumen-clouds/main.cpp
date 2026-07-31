// Fullscreen animated cloud demo client.
//
// Talks libwayland-client directly rather than through lx.wayland.client, whose
// display::connect is still not_implemented. Renders procedural fBm clouds into wl_shm
// buffers with a palette that rotates over time, and shuts down cleanly on Ctrl+C.
//
//   lumen-clouds [-t seconds]

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cmath>
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

namespace {

// Density is evaluated on a coarse lattice and bilinearly upsampled: clouds are smooth,
// so per-pixel fBm would burn CPU for detail that is not visible.
constexpr int k_low_shift = 3;
constexpr int k_palette_size = 256;
constexpr int k_buffer_count = 2;

volatile sig_atomic_t g_running = 1;

void on_signal(int) { g_running = 0; }

struct shm_buffer {
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    bool busy = false;
};

struct app {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_output* output = nullptr;

    wl_surface* surface = nullptr;
    xdg_surface* xdg_surf = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_callback* frame_cb = nullptr;

    int width = 1920;
    int height = 1080;
    bool have_output_size = false;
    bool configured = false;
    bool ready = false;
    bool need_redraw = false;

    void* pool_data = nullptr;
    size_t pool_size = 0;
    shm_buffer buffers[k_buffer_count]{};

    int low_w = 0;
    int low_h = 0;
    float* field = nullptr;
    uint32_t palette[k_palette_size]{};

    double start_time = 0.0;
    double render_seconds = 0.0;
    unsigned frames = 0;
};

double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// ── procedural clouds ───────────────────────────────────────────────────────

inline float hash2(int x, int y, int seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h) * (1.f / 4294967296.f);
}

inline float smootherstep(float t) { return t * t * (3.f - 2.f * t); }

float value_noise(float x, float y, int seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    const float tx = smootherstep(x - fx);
    const float ty = smootherstep(y - fy);

    const float n00 = hash2(ix, iy, seed);
    const float n10 = hash2(ix + 1, iy, seed);
    const float n01 = hash2(ix, iy + 1, seed);
    const float n11 = hash2(ix + 1, iy + 1, seed);

    const float a = n00 + (n10 - n00) * tx;
    const float b = n01 + (n11 - n01) * tx;
    return a + (b - a) * ty;
}

float fbm(float x, float y, int seed, int octaves) {
    float sum = 0.f;
    float amp = 0.5f;
    float norm = 0.f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * value_noise(x, y, seed + i * 131);
        norm += amp;
        x *= 2.03f;
        y *= 2.03f;
        amp *= 0.5f;
    }
    return sum / norm;
}

void hsv_to_rgb(float h, float s, float v, float* out) {
    h -= std::floor(h);
    const float sector = h * 6.f;
    const int i = static_cast<int>(sector) % 6;
    const float f = sector - std::floor(sector);
    const float p = v * (1.f - s);
    const float q = v * (1.f - s * f);
    const float t = v * (1.f - s * (1.f - f));
    switch (i) {
    case 0: out[0] = v; out[1] = t; out[2] = p; break;
    case 1: out[0] = q; out[1] = v; out[2] = p; break;
    case 2: out[0] = p; out[1] = v; out[2] = t; break;
    case 3: out[0] = p; out[1] = q; out[2] = v; break;
    case 4: out[0] = t; out[1] = p; out[2] = v; break;
    default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

void build_palette(app& a, float time) {
    // Three stops — deep sky, a vivid mid band that lights up cloud edges, and warm
    // highlights — with every hue rotating so the whole scene drifts through the spectrum.
    const float base_hue = time * 0.045f;
    float sky[3];
    float mid[3];
    float high[3];
    hsv_to_rgb(base_hue + 0.62f, 0.85f, 0.16f, sky);
    hsv_to_rgb(base_hue + 0.82f + 0.05f * std::sin(time * 0.23f), 0.72f, 0.68f, mid);
    hsv_to_rgb(base_hue + 0.06f, 0.18f, 1.00f, high);

    for (int i = 0; i < k_palette_size; ++i) {
        const float d = static_cast<float>(i) / (k_palette_size - 1);
        const float* c0 = d < 0.55f ? sky : mid;
        const float* c1 = d < 0.55f ? mid : high;
        const float local = d < 0.55f ? d / 0.55f : (d - 0.55f) / 0.45f;
        const float w = smootherstep(local);

        const float r = c0[0] + (c1[0] - c0[0]) * w;
        const float g = c0[1] + (c1[1] - c0[1]) * w;
        const float b = c0[2] + (c1[2] - c0[2]) * w;

        a.palette[i] = (static_cast<uint32_t>(r * 255.f + 0.5f) << 16) |
                       (static_cast<uint32_t>(g * 255.f + 0.5f) << 8) |
                       static_cast<uint32_t>(b * 255.f + 0.5f);
    }
}

void render(app& a, uint32_t* pixels, float time) {
    build_palette(a, time);

    // Domain warping turns plain fBm into billows and wisps; the warp and the two cloud
    // layers drift at different rates so the field keeps evolving instead of just sliding.
    const float scale_a = 0.042f;
    const float scale_b = 0.100f;
    const float warp_scale = 0.021f;

    for (int y = 0; y < a.low_h; ++y) {
        const float wy = static_cast<float>(y);
        float* row = a.field + static_cast<size_t>(y) * a.low_w;
        for (int x = 0; x < a.low_w; ++x) {
            const float wx = static_cast<float>(x);

            const float warp_x =
                fbm(wx * warp_scale + time * 0.09f, wy * warp_scale, 41, 2) - 0.5f;
            const float warp_y =
                fbm(wx * warp_scale, wy * warp_scale - time * 0.07f, 73, 2) - 0.5f;
            const float sx = wx + warp_x * 26.f;
            const float sy = wy + warp_y * 26.f;

            const float base = fbm(sx * scale_a + time * 0.42f, sy * scale_a, 1, 4);
            const float detail = fbm(sx * scale_b - time * 0.24f, sy * scale_b + time * 0.13f, 7, 3);

            float d = base * 0.74f + detail * 0.26f;
            d = (d - 0.34f) * 1.75f;
            d = d < 0.f ? 0.f : (d > 1.f ? 1.f : d);
            row[x] = smootherstep(d);
        }
    }

    // Upsample by walking each lattice cell and stepping the interpolant, which keeps the
    // inner loop to one add and one palette lookup per pixel.
    constexpr int cell = 1 << k_low_shift;
    constexpr float cell_step = 1.f / static_cast<float>(cell);
    const float inv = 1.f / static_cast<float>(cell);

    for (int y = 0; y < a.height; ++y) {
        const float fy = static_cast<float>(y) * inv;
        const int iy = static_cast<int>(fy);
        const float ty = fy - static_cast<float>(iy);
        const float* r0 = a.field + static_cast<size_t>(iy) * a.low_w;
        const float* r1 = r0 + a.low_w;
        uint32_t* out = pixels + static_cast<size_t>(y) * a.width;

        int x = 0;
        for (int i = 0; x < a.width; ++i) {
            const float left = r0[i] + (r1[i] - r0[i]) * ty;
            const float right = r0[i + 1] + (r1[i + 1] - r0[i + 1]) * ty;
            float v = left * (k_palette_size - 1);
            const float step = (right - left) * cell_step * (k_palette_size - 1);

            const int span = (a.width - x) < cell ? (a.width - x) : cell;
            for (int k = 0; k < span; ++k) {
                out[x + k] = a.palette[static_cast<int>(v)];
                v += step;
            }
            x += span;
        }
    }
}

// ── wayland plumbing ────────────────────────────────────────────────────────

void buffer_release(void* data, wl_buffer*) {
    static_cast<shm_buffer*>(data)->busy = false;
}
const wl_buffer_listener buffer_listener = {buffer_release};

int create_shm_fd(size_t size) {
    int fd = memfd_create("lumen-clouds", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool create_buffers(app& a) {
    const size_t stride = static_cast<size_t>(a.width) * 4;
    const size_t frame_size = stride * static_cast<size_t>(a.height);
    a.pool_size = frame_size * k_buffer_count;

    const int fd = create_shm_fd(a.pool_size);
    if (fd < 0) {
        std::fprintf(stderr, "lumen-clouds: memfd_create failed: %s\n", std::strerror(errno));
        return false;
    }

    a.pool_data = mmap(nullptr, a.pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a.pool_data == MAP_FAILED) {
        std::fprintf(stderr, "lumen-clouds: mmap failed: %s\n", std::strerror(errno));
        close(fd);
        a.pool_data = nullptr;
        return false;
    }

    wl_shm_pool* pool = wl_shm_create_pool(a.shm, fd, static_cast<int32_t>(a.pool_size));
    for (int i = 0; i < k_buffer_count; ++i) {
        a.buffers[i].buffer = wl_shm_pool_create_buffer(
            pool, static_cast<int32_t>(frame_size * static_cast<size_t>(i)), a.width, a.height,
            static_cast<int32_t>(stride), WL_SHM_FORMAT_XRGB8888);
        a.buffers[i].pixels = reinterpret_cast<uint32_t*>(static_cast<unsigned char*>(a.pool_data) +
                                                          frame_size * static_cast<size_t>(i));
        wl_buffer_add_listener(a.buffers[i].buffer, &buffer_listener, &a.buffers[i]);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    return true;
}

bool create_field(app& a) {
    a.low_w = (a.width >> k_low_shift) + 2;
    a.low_h = (a.height >> k_low_shift) + 2;
    a.field = new float[static_cast<size_t>(a.low_w) * a.low_h]{};
    return true;
}

void draw_frame(app& a);

void frame_done(void* data, wl_callback* cb, uint32_t) {
    auto& a = *static_cast<app*>(data);
    wl_callback_destroy(cb);
    a.frame_cb = nullptr;
    draw_frame(a);
}
const wl_callback_listener frame_listener = {frame_done};

void draw_frame(app& a) {
    // The first configure can arrive before the buffers exist, so readiness is checked
    // separately from the surface being configured.
    if (!a.configured || !a.ready || a.frame_cb)
        return;

    shm_buffer* slot = nullptr;
    for (auto& b : a.buffers) {
        if (!b.busy) {
            slot = &b;
            break;
        }
    }
    if (!slot) {
        // Both buffers are held by the compositor; retry from the poll loop.
        a.need_redraw = true;
        return;
    }
    a.need_redraw = false;

    const double render_start = now_seconds();
    render(a, slot->pixels, static_cast<float>(render_start - a.start_time));
    a.render_seconds += now_seconds() - render_start;
    ++a.frames;

    slot->busy = true;
    a.frame_cb = wl_surface_frame(a.surface);
    wl_callback_add_listener(a.frame_cb, &frame_listener, &a);

    wl_surface_attach(a.surface, slot->buffer, 0, 0);
    wl_surface_damage_buffer(a.surface, 0, 0, a.width, a.height);
    wl_surface_commit(a.surface);
}

void xdg_surface_configure(void* data, xdg_surface* surf, uint32_t serial) {
    auto& a = *static_cast<app*>(data);
    xdg_surface_ack_configure(surf, serial);
    a.configured = true;
    draw_frame(a);
}
const xdg_surface_listener xdg_surface_listener_impl = {xdg_surface_configure};

void toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    auto& a = *static_cast<app*>(data);
    // The compositor may send 0x0 to let the client pick; keep the output-sized default.
    if (width > 0 && height > 0 && !a.have_output_size) {
        a.width = width;
        a.height = height;
    }
}
void toplevel_close(void* data, xdg_toplevel*) { (void)data; g_running = 0; }
const xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close, nullptr,
                                                 nullptr};

void wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener wm_base_listener = {wm_base_ping};

void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                     const char*, int32_t) {}
void output_mode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t) {
    auto& a = *static_cast<app*>(data);
    if ((flags & WL_OUTPUT_MODE_CURRENT) && width > 0 && height > 0) {
        a.width = width;
        a.height = height;
        a.have_output_size = true;
    }
}
void output_done(void*, wl_output*) {}
void output_scale(void*, wl_output*, int32_t) {}
void output_name(void*, wl_output*, const char*) {}
void output_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {output_geometry, output_mode,        output_done,
                                            output_scale,    output_name, output_description};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
    auto& a = *static_cast<app*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        a.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        a.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        a.wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 2 ? version : 2));
        xdg_wm_base_add_listener(a.wm_base, &wm_base_listener, &a);
    } else if (std::strcmp(interface, wl_output_interface.name) == 0 && !a.output) {
        const uint32_t bind_version = version < 4 ? version : 4;
        a.output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, bind_version));
        wl_output_add_listener(a.output, &output_listener, &a);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {registry_global, registry_global_remove};

void destroy_app(app& a) {
    for (auto& b : a.buffers) {
        if (b.buffer)
            wl_buffer_destroy(b.buffer);
    }
    if (a.pool_data)
        munmap(a.pool_data, a.pool_size);
    delete[] a.field;

    if (a.frame_cb)
        wl_callback_destroy(a.frame_cb);
    if (a.toplevel)
        xdg_toplevel_destroy(a.toplevel);
    if (a.xdg_surf)
        xdg_surface_destroy(a.xdg_surf);
    if (a.surface)
        wl_surface_destroy(a.surface);
    if (a.wm_base)
        xdg_wm_base_destroy(a.wm_base);
    if (a.output)
        wl_output_destroy(a.output);
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
        if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            duration = std::atof(argv[++i]);
    }

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: poll must return EINTR so the loop can exit
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    app a{};
    a.display = wl_display_connect(nullptr);
    if (!a.display) {
        std::fprintf(stderr, "lumen-clouds: cannot connect to a Wayland display\n");
        return 1;
    }

    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    wl_display_roundtrip(a.display); // globals
    wl_display_roundtrip(a.display); // output mode

    if (!a.compositor || !a.shm || !a.wm_base) {
        std::fprintf(stderr, "lumen-clouds: missing wl_compositor, wl_shm or xdg_wm_base\n");
        destroy_app(a);
        return 1;
    }

    a.surface = wl_compositor_create_surface(a.compositor);
    a.xdg_surf = xdg_wm_base_get_xdg_surface(a.wm_base, a.surface);
    xdg_surface_add_listener(a.xdg_surf, &xdg_surface_listener_impl, &a);
    a.toplevel = xdg_surface_get_toplevel(a.xdg_surf);
    xdg_toplevel_add_listener(a.toplevel, &toplevel_listener, &a);
    xdg_toplevel_set_title(a.toplevel, "Lumen Clouds");
    xdg_toplevel_set_app_id(a.toplevel, "org.lumen.clouds");
    xdg_toplevel_set_fullscreen(a.toplevel, nullptr);
    wl_surface_commit(a.surface);
    wl_display_roundtrip(a.display);

    if (!create_buffers(a) || !create_field(a)) {
        destroy_app(a);
        return 1;
    }

    std::printf("lumen-clouds: %dx%d fullscreen, Ctrl+C to stop\n", a.width, a.height);
    std::fflush(stdout);

    a.ready = true;
    a.start_time = now_seconds();
    draw_frame(a);

    const int fd = wl_display_get_fd(a.display);
    while (g_running) {
        while (wl_display_prepare_read(a.display) != 0) {
            if (wl_display_dispatch_pending(a.display) < 0) {
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
            break;
        }

        pollfd pfd{fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, 50);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(a.display) < 0)
                break;
        } else {
            wl_display_cancel_read(a.display);
            if (ready < 0 && errno != EINTR)
                break;
        }

        if (wl_display_dispatch_pending(a.display) < 0)
            break;

        if (a.need_redraw)
            draw_frame(a);

        if (duration > 0.0 && now_seconds() - a.start_time >= duration)
            g_running = 0;
    }

    const double elapsed = now_seconds() - a.start_time;
    std::printf("\nlumen-clouds: stopped after %.1fs, %u frames (%.1f fps), %.1f ms/frame drawing\n",
                elapsed, a.frames, elapsed > 0.0 ? static_cast<double>(a.frames) / elapsed : 0.0,
                a.frames > 0 ? a.render_seconds * 1000.0 / a.frames : 0.0);
    destroy_app(a);
    return 0;
}
