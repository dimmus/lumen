// lumen-probe toplevels | capture | cursor-capture | animate: deterministic clients of screen
// capture (design-app-compat-capture Part D, F.2). Own connection; main() dispatches here.
//
//   lumen-probe toplevels [--watch S]
//       ext-foreign-toplevel-list-v1: prints every toplevel as
//         toplevel identifier=ID app_id=A title=T
//       after its `done`; with --watch, changes (`changed …`), `closed identifier=ID` and new
//       ones for S seconds, then `stop` and `finished`.
//   lumen-probe capture (--output NAME | --toplevel ID | --app-id A) [--dmabuf] [--frames N]
//       [--cursor] [--expect R,G,B@X,Y]... [--save FILE.ppm] [--timeout S] [--buffers K]
//       ext-image-copy-capture-v1 into shm (default) or LINEAR XRGB dma-buf buffers (GBM on
//       the session's dmabuf_device), K buffers in turn (default 2) with correct per-buffer
//       damage (the union of what changed since that buffer was last captured). Prints
//         constraints size=WxH shm=F,… dmabuf=F,… device=MAJ:MIN
//         frame N ready damage=R rects=x,y,wxh;… area=A transform=T time=S latency_ms=L
//         frame N failed reason=R
//       --expect checks a pixel of the last frame (buffer coordinates); `expect … OK|FAIL got=…`.
//       --save writes the last frame as a PPM. Ends with
//         capture summary frames=N ready=R failed=F full=K damage_px_mean=… stopped=0|1
//   lumen-probe cursor-capture --output NAME [--frames N] [--timeout S]
//       A pointer cursor session: prints `cursor enter|leave`, `cursor position X,Y`,
//       `cursor hotspot X,Y`, and `cursor frame N size=WxH` for each frame of its image.
//   lumen-probe animate [--size WxH] [--box N] [--interval MS] [--exit-after S] [--colour R,G,B]
//       A window (shm, solid colour, default 0,128,255) on which an N px (default 32) square
//       walks one step per interval (default 50 ms): what a capture must report as small
//       damage. Prints `animate mapped`.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#if defined(LUMEN_HAS_EGL)
#include <gbm.h>
#include <linux/dma-buf.h>
#endif

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROTOCOL_EXT_FOREIGN_TOPLEVEL_LIST_V1) &&                                  \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_CAPTURE_SOURCE_V1) &&                                   \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_COPY_CAPTURE_V1)
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#define PROBE_HAS_CAPTURE 1
#endif

int capture_main(int argc, char** argv);

#if !defined(PROBE_HAS_CAPTURE)

int capture_main(int, char**) {
    std::fprintf(stderr, "lumen-probe: built without the capture protocol glue\n");
    return 2;
}

#else

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct toplevel_info {
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    std::string identifier;
    std::string app_id;
    std::string title;
    bool announced = false;
    bool closed = false;
};

struct output_info {
    wl_output* output = nullptr;
    std::string name;
};

struct globals {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    ext_output_image_capture_source_manager_v1* output_sources = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_sources = nullptr;
    ext_image_copy_capture_manager_v1* copy = nullptr;
    std::vector<output_info*> outputs;
    std::vector<toplevel_info*> toplevels;
    bool list_finished = false;
} g;

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

bool pump(int timeout_ms) {
    while (wl_display_prepare_read(g.display) != 0)
        if (wl_display_dispatch_pending(g.display) < 0)
            return false;
    if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(g.display);
        return false;
    }
    pollfd pfd{wl_display_get_fd(g.display), POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) > 0) {
        if (wl_display_read_events(g.display) < 0)
            return false;
    } else {
        wl_display_cancel_read(g.display);
    }
    return wl_display_dispatch_pending(g.display) >= 0;
}

// ── toplevel list ────────────────────────────────────────────────────────────

const ext_foreign_toplevel_handle_v1_listener handle_listener = {
    .closed =
        [](void* d, ext_foreign_toplevel_handle_v1*) {
            auto* t = static_cast<toplevel_info*>(d);
            t->closed = true;
            std::printf("closed identifier=%s\n", t->identifier.c_str());
        },
    .done =
        [](void* d, ext_foreign_toplevel_handle_v1*) {
            auto* t = static_cast<toplevel_info*>(d);
            std::printf("%s identifier=%s app_id=%s title=%s\n",
                        t->announced ? "changed" : "toplevel", t->identifier.c_str(),
                        t->app_id.c_str(), t->title.c_str());
            t->announced = true;
        },
    .title = [](void* d, ext_foreign_toplevel_handle_v1*,
                const char* title) { static_cast<toplevel_info*>(d)->title = title ? title : ""; },
    .app_id = [](void* d, ext_foreign_toplevel_handle_v1*,
                 const char* id) { static_cast<toplevel_info*>(d)->app_id = id ? id : ""; },
    .identifier =
        [](void* d, ext_foreign_toplevel_handle_v1*, const char* id) {
            static_cast<toplevel_info*>(d)->identifier = id ? id : "";
        },
};

const ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel =
        [](void*, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* h) {
            auto* t = new toplevel_info{};
            t->handle = h;
            g.toplevels.push_back(t);
            ext_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, t);
        },
    .finished = [](void*, ext_foreign_toplevel_list_v1*) { g.list_finished = true; },
};

const wl_output_listener output_listener = {
    .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                   const char*, int32_t) {},
    .mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    .done = [](void*, wl_output*) {},
    .scale = [](void*, wl_output*, int32_t) {},
    .name = [](void* d, wl_output*, const char* name) {
        static_cast<output_info*>(d)->name = name ? name : "";
    },
    .description = [](void*, wl_output*, const char*) {},
};

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    const auto is = [&](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
    if (is(wl_compositor_interface))
        g.compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (is(wl_shm_interface))
        g.shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (is(xdg_wm_base_interface))
        g.wm = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (is(wl_seat_interface))
        g.seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 1));
    else if (is(zwp_linux_dmabuf_v1_interface) && version >= 3)
        g.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3));
    else if (is(wl_output_interface) && version >= 4) {
        auto* o = new output_info{};
        o->output = static_cast<wl_output*>(wl_registry_bind(r, name, &wl_output_interface, 4));
        wl_output_add_listener(o->output, &output_listener, o);
        g.outputs.push_back(o);
    } else if (is(ext_output_image_capture_source_manager_v1_interface))
        g.output_sources = static_cast<ext_output_image_capture_source_manager_v1*>(
            wl_registry_bind(r, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    else if (is(ext_foreign_toplevel_image_capture_source_manager_v1_interface))
        g.toplevel_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
            wl_registry_bind(r, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
                             1));
    else if (is(ext_image_copy_capture_manager_v1_interface))
        g.copy = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1));
    (void)version;
}

const wl_registry_listener registry_listener = {on_global, [](void*, wl_registry*, uint32_t) {}};

bool connect_and_bind(bool want_list) {
    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        std::fprintf(stderr, "lumen-probe: cannot connect\n");
        return false;
    }
    wl_registry* r = wl_display_get_registry(g.display);
    wl_registry_add_listener(r, &registry_listener, nullptr);
    wl_display_roundtrip(g.display);
    if (want_list) {
        // Bound only now: the toplevel events start at bind.
        struct finder {
            uint32_t name = 0;
        } f;
        static const wl_registry_listener l2 = {
            [](void* d, wl_registry*, uint32_t name, const char* iface, uint32_t) {
                if (std::strcmp(iface, ext_foreign_toplevel_list_v1_interface.name) == 0)
                    static_cast<finder*>(d)->name = name;
            },
            [](void*, wl_registry*, uint32_t) {}};
        wl_registry* r2 = wl_display_get_registry(g.display);
        wl_registry_add_listener(r2, &l2, &f);
        wl_display_roundtrip(g.display);
        if (f.name) {
            g.list = static_cast<ext_foreign_toplevel_list_v1*>(
                wl_registry_bind(r2, f.name, &ext_foreign_toplevel_list_v1_interface, 1));
            ext_foreign_toplevel_list_v1_add_listener(g.list, &list_listener, nullptr);
        }
    }
    wl_display_roundtrip(g.display);
    wl_display_roundtrip(g.display);
    return true;
}

int toplevels_main(int argc, char** argv) {
    double watch = 0.0;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--watch") && i + 1 < argc)
            watch = std::atof(argv[++i]);
    if (!connect_and_bind(true))
        return 1;
    if (!g.list) {
        std::printf("toplevels unavailable\n");
        return 1;
    }
    const double end = now_s() + watch;
    while (!g_stop && now_s() < end)
        if (!pump(100))
            return 1;
    if (watch > 0.0) {
        ext_foreign_toplevel_list_v1_stop(g.list);
        while (!g.list_finished && pump(1000)) {
        }
        std::printf("stop finished=%d\n", g.list_finished ? 1 : 0);
    }
    std::printf("toplevels count=%zu\n", g.toplevels.size());
    return 0;
}

// ── capture ──────────────────────────────────────────────────────────────────

struct capture_buffer {
    wl_buffer* buffer = nullptr;
    unsigned char* pixels = nullptr; // shm mapping, or the dma-buf's mapping when read
    std::size_t size = 0;
    unsigned stride = 0;
    int dmabuf_fd = -1;
#if defined(LUMEN_HAS_EGL)
    gbm_bo* bo = nullptr;
#endif
    /// What changed since this buffer was last captured (bounding boxes, buffer pixels).
    std::vector<int> pending; // x,y,w,h quads
    bool first = true;
};

struct session_state {
    ext_image_copy_capture_session_v1* session = nullptr;
    int width = 0;
    int height = 0;
    std::vector<uint32_t> shm_formats;
    std::vector<uint32_t> dmabuf_formats;
    dev_t device = 0;
    bool done = false;
    bool stopped = false;
    unsigned constraint_batches = 0;
} s;

struct frame_state {
    ext_image_copy_capture_frame_v1* frame = nullptr;
    bool ready = false;
    bool failed = false;
    uint32_t reason = 0;
    uint32_t transform = 0;
    std::vector<int> damage;
    double time = 0.0;
} fs;

const ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size =
        [](void*, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
            s.width = static_cast<int>(w);
            s.height = static_cast<int>(h);
            s.shm_formats.clear();
            s.dmabuf_formats.clear();
            s.done = false;
        },
    .shm_format = [](void*, ext_image_copy_capture_session_v1*,
                     uint32_t f) { s.shm_formats.push_back(f); },
    .dmabuf_device =
        [](void*, ext_image_copy_capture_session_v1*, wl_array* dev) {
            if (dev && dev->size == sizeof(dev_t))
                std::memcpy(&s.device, dev->data, sizeof(dev_t));
        },
    .dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t f,
                        wl_array*) { s.dmabuf_formats.push_back(f); },
    .done =
        [](void*, ext_image_copy_capture_session_v1*) {
            s.done = true;
            ++s.constraint_batches;
            std::printf("constraints size=%dx%d shm=", s.width, s.height);
            for (std::size_t i = 0; i < s.shm_formats.size(); ++i)
                std::printf("%s%u", i ? "," : "", s.shm_formats[i]);
            std::printf(" dmabuf=");
            for (std::size_t i = 0; i < s.dmabuf_formats.size(); ++i)
                std::printf("%s%08x", i ? "," : "", s.dmabuf_formats[i]);
            std::printf(" device=%u:%u\n", major(s.device), minor(s.device));
        },
    .stopped =
        [](void*, ext_image_copy_capture_session_v1*) {
            s.stopped = true;
            std::printf("session stopped\n");
        },
};

const ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = [](void*, ext_image_copy_capture_frame_v1*, uint32_t t) { fs.transform = t; },
    .damage =
        [](void*, ext_image_copy_capture_frame_v1*, int32_t x, int32_t y, int32_t w, int32_t h) {
            fs.damage.insert(fs.damage.end(), {x, y, w, h});
        },
    .presentation_time =
        [](void*, ext_image_copy_capture_frame_v1*, uint32_t hi, uint32_t lo, uint32_t ns) {
            fs.time = static_cast<double>((static_cast<unsigned long long>(hi) << 32) | lo) +
                      static_cast<double>(ns) / 1e9;
        },
    .ready = [](void*, ext_image_copy_capture_frame_v1*) { fs.ready = true; },
    .failed =
        [](void*, ext_image_copy_capture_frame_v1*, uint32_t reason) {
            fs.failed = true;
            fs.reason = reason;
        },
};

int shm_file(std::size_t size) {
    char name[64];
    std::snprintf(name, sizeof(name), "/lumen-probe-capture-%d", getpid());
    const int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0)
        return -1;
    return fd;
}

bool make_shm(capture_buffer& b) {
    b.stride = static_cast<unsigned>(s.width) * 4u;
    b.size = static_cast<std::size_t>(b.stride) * static_cast<unsigned>(s.height);
    const int fd = shm_file(b.size);
    if (fd < 0)
        return false;
    b.pixels = static_cast<unsigned char*>(
        mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int32_t>(b.size));
    close(fd);
    const uint32_t fmt = s.shm_formats.empty() ? WL_SHM_FORMAT_XRGB8888 : s.shm_formats[0];
    b.buffer = wl_shm_pool_create_buffer(pool, 0, s.width, s.height,
                                         static_cast<int32_t>(b.stride), fmt);
    wl_shm_pool_destroy(pool);
    // A recognisable fill, so a copy that misses a region shows.
    std::memset(b.pixels, 0x5A, b.size);
    return b.pixels != MAP_FAILED && b.buffer;
}

#if defined(LUMEN_HAS_EGL)
gbm_device* g_gbm = nullptr;

bool open_gbm() {
    if (g_gbm)
        return true;
    // The node whose dev_t the session named (render or primary).
    DIR* dir = opendir("/dev/dri");
    if (!dir)
        return false;
    int fd = -1;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.')
            continue;
        char path[300];
        std::snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
        struct stat st{};
        if (stat(path, &st) == 0 && st.st_rdev == s.device) {
            fd = open(path, O_RDWR | O_CLOEXEC);
            break;
        }
    }
    closedir(dir);
    if (fd < 0)
        return false;
    g_gbm = gbm_create_device(fd);
    return g_gbm != nullptr;
}

bool make_dmabuf(capture_buffer& b) {
    if (!g.dmabuf || s.dmabuf_formats.empty() || !open_gbm())
        return false;
    const uint32_t fmt = s.dmabuf_formats[0];
    const uint64_t linear = 0;
    b.bo = gbm_bo_create_with_modifiers2(g_gbm, static_cast<uint32_t>(s.width),
                                         static_cast<uint32_t>(s.height), fmt, &linear, 1,
                                         GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!b.bo)
        b.bo = gbm_bo_create(g_gbm, static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height),
                             fmt, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!b.bo)
        return false;
    b.dmabuf_fd = gbm_bo_get_fd(b.bo);
    b.stride = gbm_bo_get_stride(b.bo);
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(g.dmabuf);
    const uint64_t mod = gbm_bo_get_modifier(b.bo);
    zwp_linux_buffer_params_v1_add(params, b.dmabuf_fd, 0, gbm_bo_get_offset(b.bo, 0), b.stride,
                                   static_cast<uint32_t>(mod >> 32),
                                   static_cast<uint32_t>(mod & 0xFFFFFFFFu));
    b.buffer = zwp_linux_buffer_params_v1_create_immed(params, s.width, s.height, fmt, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    b.size = static_cast<std::size_t>(b.stride) * static_cast<unsigned>(s.height);
    return b.buffer != nullptr;
}
#endif

void destroy_buffer(capture_buffer& b) {
    if (b.buffer)
        wl_buffer_destroy(b.buffer);
    if (b.pixels && b.dmabuf_fd < 0)
        munmap(b.pixels, b.size);
#if defined(LUMEN_HAS_EGL)
    if (b.bo)
        gbm_bo_destroy(b.bo);
#endif
    if (b.dmabuf_fd >= 0)
        close(b.dmabuf_fd);
    b = {};
}

/// The pixels of `b` for reading (a dma-buf is mapped and synced for the read).
#if defined(LUMEN_HAS_EGL)
void* g_map_data = nullptr;
#endif

const unsigned char* read_pixels(capture_buffer& b, bool& unmap_after) {
    unmap_after = false;
    if (b.dmabuf_fd < 0)
        return b.pixels;
#if defined(LUMEN_HAS_EGL)
    if (b.bo) {
        // GBM's map (a driver may not support mmap of the dma-buf fd itself).
        uint32_t stride = 0;
        void* p = gbm_bo_map(b.bo, 0, 0, static_cast<uint32_t>(s.width),
                             static_cast<uint32_t>(s.height), GBM_BO_TRANSFER_READ, &stride,
                             &g_map_data);
        if (p) {
            b.stride = stride;
            unmap_after = true;
            return static_cast<const unsigned char*>(p);
        }
    }
#endif
    void* p = mmap(nullptr, b.size, PROT_READ, MAP_SHARED, b.dmabuf_fd, 0);
    if (p == MAP_FAILED)
        return nullptr;
#if defined(LUMEN_HAS_EGL)
    dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    (void)ioctl(b.dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
#endif
    unmap_after = true;
    return static_cast<const unsigned char*>(p);
}

void done_reading(capture_buffer& b, const unsigned char* p, bool unmap_after) {
    if (!unmap_after)
        return;
#if defined(LUMEN_HAS_EGL)
    if (b.bo && g_map_data) {
        gbm_bo_unmap(b.bo, g_map_data);
        g_map_data = nullptr;
        return;
    }
#endif
#if defined(LUMEN_HAS_EGL)
    dma_buf_sync sync{DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
    (void)ioctl(b.dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
#endif
    munmap(const_cast<unsigned char*>(p), b.size);
}

struct expectation {
    int r = 0, gch = 0, b = 0, x = 0, y = 0;
};

int capture_main_impl(int argc, char** argv) {
    const char* output_name = nullptr;
    const char* toplevel_id = nullptr;
    const char* app_id = nullptr;
    const char* save = nullptr;
    bool dmabuf = false;
    bool cursor = false;
    int frames = 1;
    int nbuffers = 2;
    double timeout = 10.0;
    std::vector<expectation> expects;
    for (int i = 2; i < argc; ++i) {
        const auto arg = [&](const char* n) { return !std::strcmp(argv[i], n) && i + 1 < argc; };
        if (arg("--output"))
            output_name = argv[++i];
        else if (arg("--toplevel"))
            toplevel_id = argv[++i];
        else if (arg("--app-id"))
            app_id = argv[++i];
        else if (!std::strcmp(argv[i], "--dmabuf"))
            dmabuf = true;
        else if (!std::strcmp(argv[i], "--shm"))
            dmabuf = false;
        else if (!std::strcmp(argv[i], "--cursor"))
            cursor = true;
        else if (arg("--frames"))
            frames = std::atoi(argv[++i]);
        else if (arg("--buffers"))
            nbuffers = std::atoi(argv[++i]);
        else if (arg("--timeout"))
            timeout = std::atof(argv[++i]);
        else if (arg("--save"))
            save = argv[++i];
        else if (arg("--expect")) {
            expectation e{};
            if (std::sscanf(argv[++i], "%d,%d,%d@%d,%d", &e.r, &e.gch, &e.b, &e.x, &e.y) == 5)
                expects.push_back(e);
        }
    }
    if (nbuffers < 1)
        nbuffers = 1;
    if (!connect_and_bind(toplevel_id || app_id))
        return 1;
    if (!g.copy || (!output_name && !toplevel_id && !app_id)) {
        std::printf("capture unavailable\n");
        return 1;
    }
    ext_image_capture_source_v1* source = nullptr;
    if (output_name) {
        for (output_info* o : g.outputs)
            if (o->name == output_name && g.output_sources)
                source = ext_output_image_capture_source_manager_v1_create_source(g.output_sources,
                                                                                  o->output);
    } else {
        for (toplevel_info* t : g.toplevels)
            if (!t->closed && ((toplevel_id && t->identifier == toplevel_id) ||
                               (app_id && t->app_id == app_id)) &&
                g.toplevel_sources) {
                source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
                    g.toplevel_sources, t->handle);
                std::printf("toplevel identifier=%s\n", t->identifier.c_str());
                break;
            }
    }
    if (!source) {
        std::printf("capture no source\n");
        return 1;
    }
    s.session = ext_image_copy_capture_manager_v1_create_session(
        g.copy, source, cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(s.session, &session_listener, nullptr);
    const double deadline = now_s() + timeout;
    while (!s.done && !s.stopped && now_s() < deadline)
        if (!pump(100))
            return 1;
    if (!s.done) {
        std::printf("capture summary frames=0 ready=0 failed=0 full=0 damage_px_mean=0 stopped=%d\n",
                    s.stopped ? 1 : 0);
        return s.stopped ? 0 : 1;
    }

    std::vector<capture_buffer> buffers(static_cast<std::size_t>(nbuffers));
    int ready = 0, failed = 0, full = 0;
    double damage_px = 0.0;
    unsigned batches = s.constraint_batches;
    capture_buffer* last = nullptr;
    for (int n = 0; n < frames && !g_stop && now_s() < deadline && !s.stopped; ++n) {
        if (s.constraint_batches != batches) {
            // New constraints: every buffer is made again.
            for (auto& b : buffers)
                destroy_buffer(b);
            batches = s.constraint_batches;
        }
        capture_buffer& b = buffers[static_cast<std::size_t>(n % nbuffers)];
        if (!b.buffer) {
            bool ok = false;
#if defined(LUMEN_HAS_EGL)
            if (dmabuf)
                ok = make_dmabuf(b);
#endif
            if (!dmabuf)
                ok = make_shm(b);
            if (!ok) {
                std::printf("capture cannot make a %s buffer\n", dmabuf ? "dma-buf" : "shm");
                return 1;
            }
        }
        fs = {};
        fs.frame = ext_image_copy_capture_session_v1_create_frame(s.session);
        ext_image_copy_capture_frame_v1_add_listener(fs.frame, &frame_listener, nullptr);
        ext_image_copy_capture_frame_v1_attach_buffer(fs.frame, b.buffer);
        if (b.first) {
            ext_image_copy_capture_frame_v1_damage_buffer(fs.frame, 0, 0, s.width, s.height);
        } else {
            for (std::size_t k = 0; k + 3 < b.pending.size(); k += 4)
                ext_image_copy_capture_frame_v1_damage_buffer(fs.frame, b.pending[k],
                                                              b.pending[k + 1], b.pending[k + 2],
                                                              b.pending[k + 3]);
        }
        const double asked = now_s();
        ext_image_copy_capture_frame_v1_capture(fs.frame);
        while (!fs.ready && !fs.failed && now_s() < deadline && !g_stop)
            if (!pump(100))
                return 1;
        if (fs.ready) {
            ++ready;
            b.first = false;
            b.pending.clear();
            // What this frame changed is stale in every other buffer.
            for (auto& other : buffers)
                if (&other != &b)
                    other.pending.insert(other.pending.end(), fs.damage.begin(), fs.damage.end());
            double area = 0.0;
            bool is_full = false;
            std::string rects;
            for (std::size_t k = 0; k + 3 < fs.damage.size(); k += 4) {
                area += static_cast<double>(fs.damage[k + 2]) * fs.damage[k + 3];
                if (fs.damage[k] == 0 && fs.damage[k + 1] == 0 && fs.damage[k + 2] >= s.width &&
                    fs.damage[k + 3] >= s.height)
                    is_full = true;
                char r[64];
                std::snprintf(r, sizeof(r), "%s%d,%d,%dx%d", rects.empty() ? "" : ";",
                              fs.damage[k], fs.damage[k + 1], fs.damage[k + 2], fs.damage[k + 3]);
                rects += r;
            }
            full += is_full ? 1 : 0;
            damage_px += area;
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            std::printf("frame %d ready damage=%s rects=%s area=%.0f transform=%u time=%.6f "
                        "latency_ms=%.2f\n",
                        n + 1, is_full ? "full" : "partial", rects.c_str(), area, fs.transform,
                        fs.time, (now_s() - asked) * 1000.0);
            last = &b;
        } else if (fs.failed) {
            ++failed;
            std::printf("frame %d failed reason=%u\n", n + 1, fs.reason);
            if (fs.reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
                // The client's answer: wait for the new constraints, make buffers again.
                while (s.constraint_batches == batches && !s.stopped && now_s() < deadline)
                    if (!pump(100))
                        return 1;
            }
        } else {
            std::printf("frame %d timeout\n", n + 1);
            ext_image_copy_capture_frame_v1_destroy(fs.frame);
            break;
        }
        ext_image_copy_capture_frame_v1_destroy(fs.frame);
    }
    if (last) {
        bool unmap_after = false;
        const unsigned char* px = read_pixels(*last, unmap_after);
        if (px) {
            for (const expectation& e : expects) {
                if (e.x < 0 || e.y < 0 || e.x >= s.width || e.y >= s.height) {
                    std::printf("expect %d,%d,%d@%d,%d FAIL outside\n", e.r, e.gch, e.b, e.x, e.y);
                    continue;
                }
                const unsigned char* p =
                    px + static_cast<std::size_t>(e.y) * last->stride + static_cast<std::size_t>(e.x) * 4u;
                // XRGB8888 / ARGB8888 / XR24: B, G, R in memory.
                const int r = p[2], gg = p[1], bb = p[0];
                const bool ok = std::abs(r - e.r) <= 2 && std::abs(gg - e.gch) <= 2 &&
                                std::abs(bb - e.b) <= 2;
                std::printf("expect %d,%d,%d@%d,%d %s got=%d,%d,%d\n", e.r, e.gch, e.b, e.x, e.y,
                            ok ? "OK" : "FAIL", r, gg, bb);
            }
            if (save) {
                if (FILE* f = std::fopen(save, "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", s.width, s.height);
                    for (int y = 0; y < s.height; ++y)
                        for (int x = 0; x < s.width; ++x) {
                            const unsigned char* p = px + static_cast<std::size_t>(y) * last->stride +
                                                     static_cast<std::size_t>(x) * 4u;
                            const unsigned char rgb[3] = {p[2], p[1], p[0]};
                            std::fwrite(rgb, 1, 3, f);
                        }
                    std::fclose(f);
                }
            }
            done_reading(*last, px, unmap_after);
        }
    }
    std::printf("capture summary frames=%d ready=%d failed=%d full=%d damage_px_mean=%.0f "
                "stopped=%d\n",
                frames, ready, failed, full, ready ? damage_px / ready : 0.0, s.stopped ? 1 : 0);
    for (auto& b : buffers)
        destroy_buffer(b);
    ext_image_copy_capture_session_v1_destroy(s.session);
    wl_display_roundtrip(g.display);
    return 0;
}

// ── cursor sessions ──────────────────────────────────────────────────────────

int cursor_main(int argc, char** argv) {
    const char* output_name = nullptr;
    int frames = 1;
    double timeout = 10.0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--output") && i + 1 < argc)
            output_name = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--timeout") && i + 1 < argc)
            timeout = std::atof(argv[++i]);
    }
    if (!connect_and_bind(false) || !g.copy || !g.output_sources || !g.seat) {
        std::printf("cursor-capture unavailable\n");
        return 1;
    }
    g.pointer = wl_seat_get_pointer(g.seat);
    ext_image_capture_source_v1* source = nullptr;
    for (output_info* o : g.outputs)
        if (!output_name || o->name == output_name)
            source = ext_output_image_capture_source_manager_v1_create_source(g.output_sources,
                                                                              o->output);
    if (!source)
        return 1;
    ext_image_copy_capture_cursor_session_v1* cs =
        ext_image_copy_capture_manager_v1_create_pointer_cursor_session(g.copy, source, g.pointer);
    static const ext_image_copy_capture_cursor_session_v1_listener cl = {
        .enter = [](void*, ext_image_copy_capture_cursor_session_v1*) { std::printf("cursor enter\n"); },
        .leave = [](void*, ext_image_copy_capture_cursor_session_v1*) { std::printf("cursor leave\n"); },
        .position = [](void*, ext_image_copy_capture_cursor_session_v1*, int32_t x,
                       int32_t y) { std::printf("cursor position %d,%d\n", x, y); },
        .hotspot = [](void*, ext_image_copy_capture_cursor_session_v1*, int32_t x,
                      int32_t y) { std::printf("cursor hotspot %d,%d\n", x, y); },
    };
    ext_image_copy_capture_cursor_session_v1_add_listener(cs, &cl, nullptr);
    s.session = ext_image_copy_capture_cursor_session_v1_get_capture_session(cs);
    ext_image_copy_capture_session_v1_add_listener(s.session, &session_listener, nullptr);
    const double deadline = now_s() + timeout;
    while (!s.done && !s.stopped && now_s() < deadline)
        if (!pump(100))
            return 1;
    int got = 0;
    capture_buffer b{};
    unsigned batches = 0;
    while (got < frames && !s.stopped && now_s() < deadline && !g_stop) {
        if (!b.buffer || batches != s.constraint_batches) {
            destroy_buffer(b);
            if (std::find(s.shm_formats.begin(), s.shm_formats.end(), WL_SHM_FORMAT_ARGB8888) ==
                s.shm_formats.end())
                return 1;
            s.shm_formats.insert(s.shm_formats.begin(), WL_SHM_FORMAT_ARGB8888);
            if (!make_shm(b))
                return 1;
            batches = s.constraint_batches;
        }
        fs = {};
        fs.frame = ext_image_copy_capture_session_v1_create_frame(s.session);
        ext_image_copy_capture_frame_v1_add_listener(fs.frame, &frame_listener, nullptr);
        ext_image_copy_capture_frame_v1_attach_buffer(fs.frame, b.buffer);
        ext_image_copy_capture_frame_v1_damage_buffer(fs.frame, 0, 0, s.width, s.height);
        ext_image_copy_capture_frame_v1_capture(fs.frame);
        while (!fs.ready && !fs.failed && now_s() < deadline && !g_stop)
            if (!pump(100))
                return 1;
        if (fs.ready) {
            ++got;
            unsigned opaque = 0;
            for (std::size_t k = 3; k < b.size; k += 4)
                opaque += b.pixels[k] != 0;
            std::printf("cursor frame %d size=%dx%d visible_px=%u\n", got, s.width, s.height, opaque);
        } else if (fs.failed) {
            std::printf("cursor frame failed reason=%u\n", fs.reason);
        }
        ext_image_copy_capture_frame_v1_destroy(fs.frame);
        if (!fs.ready)
            break;
    }
    // Events keep coming while the pointer moves.
    while (now_s() < deadline && !g_stop && !s.stopped)
        if (!pump(100))
            break;
    destroy_buffer(b);
    return 0;
}

// ── animate ──────────────────────────────────────────────────────────────────

int animate_main(int argc, char** argv) {
    int w = 400, h = 300, box = 32, interval = 50;
    int cr = 0, cg = 128, cb = 255;
    double exit_after = 30.0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!std::strcmp(argv[i], "--box") && i + 1 < argc)
            box = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--interval") && i + 1 < argc)
            interval = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--exit-after") && i + 1 < argc)
            exit_after = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--colour") && i + 1 < argc)
            std::sscanf(argv[++i], "%d,%d,%d", &cr, &cg, &cb);
    }
    if (!connect_and_bind(false) || !g.compositor || !g.wm || !g.shm)
        return 1;
    static const xdg_wm_base_listener wl = {
        [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }};
    xdg_wm_base_add_listener(g.wm, &wl, nullptr);
    wl_surface* surface = wl_compositor_create_surface(g.compositor);
    xdg_surface* xs = xdg_wm_base_get_xdg_surface(g.wm, surface);
    struct cfg_state {
        uint32_t serial = 0;
        bool configured = false;
    } cfg;
    static const xdg_surface_listener xl = {[](void* d, xdg_surface*, uint32_t serial) {
        static_cast<cfg_state*>(d)->serial = serial;
        static_cast<cfg_state*>(d)->configured = true;
    }};
    xdg_surface_add_listener(xs, &xl, &cfg);
    xdg_toplevel* top = xdg_surface_get_toplevel(xs);
    static const xdg_toplevel_listener tl = {
        [](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {},
        [](void*, xdg_toplevel*) { g_stop = 1; },
        [](void*, xdg_toplevel*, int32_t, int32_t) {},
        [](void*, xdg_toplevel*, wl_array*) {}};
    xdg_toplevel_add_listener(top, &tl, nullptr);
    xdg_toplevel_set_title(top, "lumen-probe animate");
    xdg_toplevel_set_app_id(top, "lumen-probe-animate");
    wl_surface_commit(surface);
    while (!cfg.configured && pump(1000)) {
    }
    xdg_surface_ack_configure(xs, cfg.serial);
    // Two buffers in turn; each frame repaints the old and the new square's place.
    s.width = w;
    s.height = h;
    s.shm_formats = {WL_SHM_FORMAT_XRGB8888};
    capture_buffer bufs[2];
    for (auto& b : bufs) {
        if (!make_shm(b))
            return 1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned char* p = b.pixels + static_cast<std::size_t>(y) * b.stride + x * 4u;
                p[0] = static_cast<unsigned char>(cb);
                p[1] = static_cast<unsigned char>(cg);
                p[2] = static_cast<unsigned char>(cr);
                p[3] = 0xFF;
            }
    }
    const auto paint = [&](capture_buffer& b, int bx, int by, bool on) {
        for (int y = by; y < by + box && y < h; ++y)
            for (int x = bx; x < bx + box && x < w; ++x) {
                unsigned char* p = b.pixels + static_cast<std::size_t>(y) * b.stride + x * 4u;
                p[0] = on ? 0 : static_cast<unsigned char>(cb);
                p[1] = on ? 255 : static_cast<unsigned char>(cg);
                p[2] = on ? 255 : static_cast<unsigned char>(cr);
                p[3] = 0xFF;
            }
    };
    int old_pos[2] = {-1, -1}; // per buffer: the square's position in it
    const double end = now_s() + exit_after;
    int step = 0;
    std::printf("animate mapped\n");
    double next = now_s();
    while (!g_stop && now_s() < end) {
        if (now_s() >= next) {
            capture_buffer& b = bufs[step % 2];
            const int span = (w - box) > 0 ? (w - box) : 1;
            const int x = (step * 8) % span;
            const int y = (h - box) / 2;
            if (old_pos[step % 2] >= 0)
                paint(b, old_pos[step % 2], y, false);
            paint(b, x, y, true);
            old_pos[step % 2] = x;
            wl_surface_attach(surface, b.buffer, 0, 0);
            const int dx0 = x - 16 > 0 ? x - 16 : 0;
            wl_surface_damage_buffer(surface, dx0, y, box + 32, box);
            if (step < 2 || x < 16)
                wl_surface_damage_buffer(surface, 0, 0, w, h); // first frames, wrap-around
            wl_surface_commit(surface);
            ++step;
            next += interval / 1000.0;
        }
        if (!pump(5))
            break;
    }
    return 0;
}

} // namespace

int capture_main(int argc, char** argv) {
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });
    if (!std::strcmp(argv[1], "toplevels"))
        return toplevels_main(argc, argv);
    if (!std::strcmp(argv[1], "capture"))
        return capture_main_impl(argc, argv);
    if (!std::strcmp(argv[1], "cursor-capture"))
        return cursor_main(argc, argv);
    if (!std::strcmp(argv[1], "animate"))
        return animate_main(argc, argv);
    return 2;
}

#endif // PROBE_HAS_CAPTURE
