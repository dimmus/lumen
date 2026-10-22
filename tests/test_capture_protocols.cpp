// Screen capture in-process (design-app-compat-capture Part D): ext-foreign-toplevel-list-v1,
// ext-image-capture-source-v1, ext-image-copy-capture-v1 and wlr-screencopy-unstable-v1 v3
// against a real compositor on one virtual output composited on the CPU (`--present cpu
// --virtual-outputs`: frames are composited only into capture clients' buffers). Every case
// is its own libwayland-client connection (a protocol error is fatal to it); the test drives
// the compositor's ticks itself (UI then render) so frames happen when it says.
//
// Covers: the globals and versions; the toplevel list (handles at bind and at map, identifier
// / title / app id / done, title changes, closed at unmap, a new identifier per mapping, stop
// → finished); output capture into shm (constraints, the first frame full, the pixels of a
// window, a damage-only second frame waiting for the change); window capture of a covered
// window (its own pixels, not the cover's); every protocol error (invalid_option,
// duplicate_frame, no_buffer, invalid_buffer_damage, already_captured both ways,
// duplicate_session, screencopy already_used and invalid_buffer); buffer_constraints for a
// wrong-size buffer; stopped and failed(stopped) when the window unmaps; the session lock
// (the curtain, then nothing until unlock, then a full frame); wlr-screencopy copy and
// copy_with_damage (waits for damage, reports it).
#include <errno.h>
#include <stdio.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_EXT_FOREIGN_TOPLEVEL_LIST_V1) && \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_CAPTURE_SOURCE_V1) &&                                     \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_COPY_CAPTURE_V1) &&                                       \
    defined(LUMEN_PROTOCOL_WLR_SCREENCOPY_UNSTABLE_V1) && defined(LUMEN_PROTOCOL_EXT_SESSION_LOCK_V1)
#include "xdg-shell-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#define LUMEN_TEST_CAPTURE 1
#endif
#endif
#include <stdint.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.compositor;

#if !defined(LUMEN_TEST_CAPTURE)

int main() {
    std::printf("SKIP: needs libwayland and the capture protocols' glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-capture";
constexpr int k_out_w = 640;
constexpr int k_out_h = 480;

int g_failed = 0;
int g_ran = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 0;

/// One frame of the compositor: its UI tick, then its render tick.
void tick() {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scheduler::frame_tick t{};
    t.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    t.delta_seconds = 1.0 / 30.0;
    t.frame_index = static_cast<unsigned>(++g_frame);
    g_comp->tick_ui(t);
    g_comp->wayland().flush();
    (void)g_comp->render_and_wait();
    g_comp->run_posted_ui_tasks();
}

void check(const char* name, bool ok, const std::string& detail) {
    ++g_ran;
    std::printf("  %s  %s — %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

struct client {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    wl_output* output = nullptr;
    wl_seat* seat = nullptr;
    ext_output_image_capture_source_manager_v1* output_sources = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_sources = nullptr;
    ext_image_copy_capture_manager_v1* copy = nullptr;
    zwlr_screencopy_manager_v1* screencopy = nullptr;
    ext_session_lock_manager_v1* lock_manager = nullptr;
    uint32_t list_name = 0;
    std::vector<std::pair<std::string, uint32_t>> globals;
};

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    c->globals.emplace_back(iface, version);
    const auto is = [&](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
    if (is(wl_compositor_interface))
        c->compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (is(xdg_wm_base_interface))
        c->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (is(wl_shm_interface))
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (is(wl_output_interface) && !c->output)
        c->output = static_cast<wl_output*>(wl_registry_bind(r, name, &wl_output_interface, 1));
    else if (is(wl_seat_interface))
        c->seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 1));
    else if (is(ext_output_image_capture_source_manager_v1_interface))
        c->output_sources = static_cast<ext_output_image_capture_source_manager_v1*>(
            wl_registry_bind(r, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    else if (is(ext_foreign_toplevel_image_capture_source_manager_v1_interface))
        c->toplevel_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
            wl_registry_bind(r, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
                             1));
    else if (is(ext_image_copy_capture_manager_v1_interface))
        c->copy = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1));
    else if (is(zwlr_screencopy_manager_v1_interface))
        c->screencopy = static_cast<zwlr_screencopy_manager_v1*>(
            wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 3));
    else if (is(ext_session_lock_manager_v1_interface))
        c->lock_manager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1));
    else if (is(ext_foreign_toplevel_list_v1_interface))
        c->list_name = name;
}
const wl_registry_listener registry_listener = {on_global, [](void*, wl_registry*, uint32_t) {}};

const xdg_wm_base_listener wm_listener = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }};

bool connect(client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    wl_registry* r = wl_display_get_registry(c.display);
    wl_registry_add_listener(r, &registry_listener, &c);
    if (wl_display_roundtrip(c.display) < 0 || wl_display_roundtrip(c.display) < 0)
        return false;
    if (c.wm_base)
        xdg_wm_base_add_listener(c.wm_base, &wm_listener, nullptr);
    return true;
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

/// Ticks and dispatches until `done()` or `ticks` frames passed.
bool run_until(client& c, const std::function<bool()>& done, unsigned ticks = 40) {
    for (unsigned i = 0; i < ticks; ++i) {
        if (done())
            return true;
        tick();
        (void)wl_display_roundtrip(c.display);
    }
    return done();
}

// ── buffers and windows ──

struct shm_buffer {
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    std::size_t size = 0;
};

shm_buffer make_buffer(client& c, int w, int h, uint32_t argb,
                       uint32_t format = WL_SHM_FORMAT_XRGB8888) {
    shm_buffer b{};
    const int stride = w * 4;
    b.size = static_cast<std::size_t>(stride) * h;
    b.width = w;
    b.height = h;
    const int fd = memfd_create("lumen-test-capture", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(b.size)) != 0)
        return b;
    void* p = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return b;
    }
    b.pixels = static_cast<uint32_t*>(p);
    for (int i = 0; i < w * h; ++i)
        b.pixels[i] = argb;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, static_cast<int32_t>(b.size));
    b.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    uint32_t serial = 0;
    shm_buffer content{};
};

const xdg_surface_listener xdg_listener = {
    [](void* data, xdg_surface*, uint32_t serial) { static_cast<window*>(data)->serial = serial; }};

void map_window(client& c, window& w, const char* app_id, int width, int height, uint32_t argb,
                const char* title = "capture test") {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
    xdg_toplevel_set_title(w.toplevel, title);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(w.xdg, w.serial);
    w.content = make_buffer(c, width, height, argb);
    wl_surface_attach(w.surface, w.content.buffer, 0, 0);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    tick();
    (void)wl_display_roundtrip(c.display);
}

/// The window geometry of the newest mapped toplevel with `app_id` (global = output pixels
/// here: one output at the origin, scale 1).
lx::rect2i geometry_of(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::rect2i out{};
    unsigned long long best = 0;
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.mapped && r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > best) {
            best = r.id.id();
            out = r.geometry;
        }
    });
    return out;
}

// ── capture sessions ──

struct session {
    ext_image_copy_capture_session_v1* session = nullptr;
    int width = 0;
    int height = 0;
    std::vector<uint32_t> shm;
    std::vector<uint32_t> dmabuf;
    unsigned dones = 0;
    bool stopped = false;
};

const ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size =
        [](void* d, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
            auto* s = static_cast<session*>(d);
            s->width = static_cast<int>(w);
            s->height = static_cast<int>(h);
            s->shm.clear();
            s->dmabuf.clear();
        },
    .shm_format = [](void* d, ext_image_copy_capture_session_v1*,
                     uint32_t f) { static_cast<session*>(d)->shm.push_back(f); },
    .dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
    .dmabuf_format = [](void* d, ext_image_copy_capture_session_v1*, uint32_t f,
                        wl_array*) { static_cast<session*>(d)->dmabuf.push_back(f); },
    .done = [](void* d, ext_image_copy_capture_session_v1*) { static_cast<session*>(d)->dones++; },
    .stopped = [](void* d, ext_image_copy_capture_session_v1*) { static_cast<session*>(d)->stopped = true; },
};

struct frame {
    ext_image_copy_capture_frame_v1* frame = nullptr;
    bool ready = false;
    bool failed = false;
    uint32_t reason = ~0u;
    std::vector<int> damage;
    bool has_time = false;
    bool has_transform = false;
};

const ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = [](void* d, ext_image_copy_capture_frame_v1*,
                    uint32_t) { static_cast<frame*>(d)->has_transform = true; },
    .damage =
        [](void* d, ext_image_copy_capture_frame_v1*, int32_t x, int32_t y, int32_t w, int32_t h) {
            static_cast<frame*>(d)->damage.insert(static_cast<frame*>(d)->damage.end(), {x, y, w, h});
        },
    .presentation_time = [](void* d, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t,
                            uint32_t) { static_cast<frame*>(d)->has_time = true; },
    .ready = [](void* d, ext_image_copy_capture_frame_v1*) { static_cast<frame*>(d)->ready = true; },
    .failed =
        [](void* d, ext_image_copy_capture_frame_v1*, uint32_t reason) {
            static_cast<frame*>(d)->failed = true;
            static_cast<frame*>(d)->reason = reason;
        },
};

void open_session(client& c, session& s, ext_image_capture_source_v1* source, uint32_t options = 0) {
    s.session = ext_image_copy_capture_manager_v1_create_session(c.copy, source, options);
    ext_image_copy_capture_session_v1_add_listener(s.session, &session_listener, &s);
    (void)wl_display_roundtrip(c.display);
}

ext_image_capture_source_v1* output_source(client& c) {
    return ext_output_image_capture_source_manager_v1_create_source(c.output_sources, c.output);
}

/// Captures one frame of `s` into `b` with `damage` (x,y,w,h quads) and waits for the answer.
frame capture(client& c, session& s, shm_buffer& b, const std::vector<int>& damage,
              unsigned ticks = 40) {
    frame f{};
    f.frame = ext_image_copy_capture_session_v1_create_frame(s.session);
    ext_image_copy_capture_frame_v1_add_listener(f.frame, &frame_listener, &f);
    ext_image_copy_capture_frame_v1_attach_buffer(f.frame, b.buffer);
    for (std::size_t i = 0; i + 3 < damage.size(); i += 4)
        ext_image_copy_capture_frame_v1_damage_buffer(f.frame, damage[i], damage[i + 1],
                                                      damage[i + 2], damage[i + 3]);
    ext_image_copy_capture_frame_v1_capture(f.frame);
    (void)run_until(c, [&] { return f.ready || f.failed; }, ticks);
    // One frame object per session at a time: this one is done with.
    ext_image_copy_capture_frame_v1_destroy(f.frame);
    f.frame = nullptr;
    return f;
}

uint32_t pixel(const shm_buffer& b, int x, int y) {
    return b.pixels[static_cast<std::size_t>(y) * b.width + x] & 0x00FFFFFFu;
}

std::string hex(uint32_t v) {
    char s[16];
    std::snprintf(s, sizeof(s), "%06x", v);
    return s;
}

// ── the toplevel list ──

struct handle_info {
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    std::string identifier;
    std::string title;
    std::string app_id;
    unsigned dones = 0;
    bool closed = false;
};

struct list_state {
    ext_foreign_toplevel_list_v1* list = nullptr;
    std::vector<handle_info*> handles;
    bool finished = false;
};

const ext_foreign_toplevel_handle_v1_listener handle_listener = {
    .closed = [](void* d, ext_foreign_toplevel_handle_v1*) { static_cast<handle_info*>(d)->closed = true; },
    .done = [](void* d, ext_foreign_toplevel_handle_v1*) { static_cast<handle_info*>(d)->dones++; },
    .title = [](void* d, ext_foreign_toplevel_handle_v1*,
                const char* t) { static_cast<handle_info*>(d)->title = t; },
    .app_id = [](void* d, ext_foreign_toplevel_handle_v1*,
                 const char* a) { static_cast<handle_info*>(d)->app_id = a; },
    .identifier = [](void* d, ext_foreign_toplevel_handle_v1*,
                     const char* id) { static_cast<handle_info*>(d)->identifier = id; },
};

const ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel =
        [](void* d, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* h) {
            auto* info = new handle_info{};
            info->handle = h;
            ext_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, info);
            static_cast<list_state*>(d)->handles.push_back(info);
        },
    .finished = [](void* d, ext_foreign_toplevel_list_v1*) { static_cast<list_state*>(d)->finished = true; },
};

void bind_list(client& c, list_state& l) {
    wl_registry* r = wl_display_get_registry(c.display);
    l.list = static_cast<ext_foreign_toplevel_list_v1*>(
        wl_registry_bind(r, c.list_name, &ext_foreign_toplevel_list_v1_interface, 1));
    ext_foreign_toplevel_list_v1_add_listener(l.list, &list_listener, &l);
    (void)wl_display_roundtrip(c.display);
}

handle_info* find_handle(list_state& l, const char* app_id) {
    for (auto it = l.handles.rbegin(); it != l.handles.rend(); ++it)
        if ((*it)->app_id == app_id && !(*it)->closed)
            return *it;
    return nullptr;
}

bool hex32(const std::string& s) {
    if (s.size() != 32)
        return false;
    for (char ch : s)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

// ── cases ──

void globals_case() {
    client c{};
    if (!connect(c)) {
        check("globals", false, "could not connect");
        return;
    }
    const auto version = [&](const char* name) -> uint32_t {
        for (const auto& g : c.globals)
            if (g.first == name)
                return g.second;
        return 0;
    };
    check("globals: ext_foreign_toplevel_list_v1 v1", version("ext_foreign_toplevel_list_v1") == 1, "");
    check("globals: ext_output_image_capture_source_manager_v1 v1",
          version("ext_output_image_capture_source_manager_v1") == 1, "");
    check("globals: ext_foreign_toplevel_image_capture_source_manager_v1 v1",
          version("ext_foreign_toplevel_image_capture_source_manager_v1") == 1, "");
    check("globals: ext_image_copy_capture_manager_v1 v1",
          version("ext_image_copy_capture_manager_v1") == 1, "");
    check("globals: zwlr_screencopy_manager_v1 v3", version("zwlr_screencopy_manager_v1") == 3, "");
    (void)finish(c);
}

void list_cases() {
    client a{};
    client lc{};
    if (!connect(a) || !connect(lc)) {
        check("toplevel list", false, "could not connect");
        return;
    }
    window w{};
    map_window(a, w, "lumen-test-list", 120, 80, 0xFF102030u, "first title");
    list_state l{};
    bind_list(lc, l);
    handle_info* h = find_handle(l, "lumen-test-list");
    check("list: a mapped window is announced at bind with identifier, title, app id, done",
          h && hex32(h->identifier) && h->title == "first title" && h->dones == 1,
          h ? h->identifier + " / " + h->title : std::string("no handle"));
    const std::string first_id = h ? h->identifier : "";

    xdg_toplevel_set_title(w.toplevel, "second title");
    (void)wl_display_roundtrip(a.display);
    (void)wl_display_roundtrip(lc.display);
    check("list: a title change is sent with done", h && h->title == "second title" && h->dones == 2,
          h ? h->title + " dones=" + std::to_string(h->dones) : "");

    // Unmap (null attach): closed; a remap is a new handle with a new identifier.
    wl_surface_attach(w.surface, nullptr, 0, 0);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(a.display);
    tick();
    (void)wl_display_roundtrip(lc.display);
    check("list: unmap sends closed", h && h->closed, "");
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(a.display);
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_attach(w.surface, w.content.buffer, 0, 0);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(a.display);
    tick();
    (void)wl_display_roundtrip(lc.display);
    handle_info* h2 = find_handle(l, "lumen-test-list");
    check("list: a remapped window is a new handle with a new identifier",
          h2 && h2 != h && hex32(h2->identifier) && h2->identifier != first_id,
          h2 ? h2->identifier : std::string("no handle"));

    ext_foreign_toplevel_list_v1_stop(l.list);
    (void)wl_display_roundtrip(lc.display);
    const std::size_t before = l.handles.size();
    window w2{};
    map_window(a, w2, "lumen-test-list-2", 60, 40, 0xFF203040u);
    (void)wl_display_roundtrip(lc.display);
    check("list: stop is answered by finished, and no toplevel follows",
          l.finished && l.handles.size() == before, "handles=" + std::to_string(l.handles.size()));
    xdg_toplevel_destroy(w2.toplevel);
    xdg_surface_destroy(w2.xdg);
    wl_surface_destroy(w2.surface);
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    (void)wl_display_roundtrip(a.display);
    tick();
    (void)finish(a);
    (void)finish(lc);
}

void output_cases() {
    client c{};
    if (!connect(c) || !c.copy || !c.output_sources) {
        check("output capture", false, "no capture globals");
        return;
    }
    window w{};
    map_window(c, w, "lumen-test-blue", 160, 120, 0xFF0000FFu);
    const lx::rect2i g = geometry_of("lumen-test-blue");
    session s{};
    open_session(c, s, output_source(c));
    check("output: constraints buffer_size, shm XRGB then ARGB, no dma-buf on the CPU path, done",
          s.width == k_out_w && s.height == k_out_h && s.shm.size() == 2 &&
              s.shm[0] == WL_SHM_FORMAT_XRGB8888 && s.shm[1] == WL_SHM_FORMAT_ARGB8888 &&
              s.dmabuf.empty() && s.dones == 1,
          std::to_string(s.width) + "x" + std::to_string(s.height));
    shm_buffer b = make_buffer(c, s.width, s.height, 0xFF5A5A5Au);
    frame f1 = capture(c, s, b, {0, 0, s.width, s.height});
    const bool full = f1.damage.size() == 4 && f1.damage[0] == 0 && f1.damage[1] == 0 &&
                      f1.damage[2] == s.width && f1.damage[3] == s.height;
    check("output: the first frame is ready at once with full damage, transform and time",
          f1.ready && full && f1.has_time && f1.has_transform, f1.ready ? "ready" : "not ready");
    const uint32_t inside = pixel(b, g.x + g.width / 2, g.y + g.height / 2);
    const uint32_t outside = pixel(b, g.x + g.width + 5 < k_out_w ? g.x + g.width + 5 : 2, 2);
    check("output: the window's pixels and the black around it", inside == 0x0000FFu && outside == 0,
          "inside=" + hex(inside) + " outside=" + hex(outside));

    // A second frame waits for a change; a 20x10 change is reported as (at least) that.
    frame f2{};
    f2.frame = ext_image_copy_capture_session_v1_create_frame(s.session);
    ext_image_copy_capture_frame_v1_add_listener(f2.frame, &frame_listener, &f2);
    ext_image_copy_capture_frame_v1_attach_buffer(f2.frame, b.buffer);
    ext_image_copy_capture_frame_v1_capture(f2.frame);
    for (int i = 0; i < 4; ++i) {
        tick();
        (void)wl_display_roundtrip(c.display);
    }
    const bool waited = !f2.ready && !f2.failed;
    for (int y = 10; y < 20; ++y)
        for (int x = 30; x < 50; ++x)
            w.content.pixels[static_cast<std::size_t>(y) * w.content.width + x] = 0xFFFF0000u;
    wl_surface_attach(w.surface, w.content.buffer, 0, 0);
    wl_surface_damage_buffer(w.surface, 30, 10, 20, 10);
    wl_surface_commit(w.surface);
    (void)run_until(c, [&] { return f2.ready || f2.failed; });
    long area = 0;
    bool covers = false;
    for (std::size_t i = 0; i + 3 < f2.damage.size(); i += 4) {
        area += static_cast<long>(f2.damage[i + 2]) * f2.damage[i + 3];
        covers = covers || (f2.damage[i] <= g.x + 30 && f2.damage[i + 1] <= g.y + 10 &&
                            f2.damage[i] + f2.damage[i + 2] >= g.x + 50 &&
                            f2.damage[i + 1] + f2.damage[i + 3] >= g.y + 20);
    }
    check("output: a later frame waits for the source to change, then reports only the change",
          waited && f2.ready && covers && area < 2000,
          "waited=" + std::to_string(waited) + " area=" + std::to_string(area));
    check("output: the change is in the buffer",
          pixel(b, g.x + 40, g.y + 15) == 0xFF0000u && pixel(b, g.x + 10, g.y + 50) == 0x0000FFu,
          hex(pixel(b, g.x + 40, g.y + 15)));
    ext_image_copy_capture_frame_v1_destroy(f2.frame);

    // A buffer of the wrong size: failed(buffer_constraints).
    shm_buffer small = make_buffer(c, 32, 32, 0);
    frame f3 = capture(c, s, small, {0, 0, 32, 32});
    check("output: a buffer that does not fit fails with buffer_constraints",
          f3.failed && f3.reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS,
          "reason=" + std::to_string(f3.reason));
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    tick();
    (void)finish(c);
}

void toplevel_cases() {
    client c{};
    if (!connect(c) || !c.toplevel_sources || !c.list_name) {
        check("toplevel capture", false, "no toplevel source manager");
        return;
    }
    window under{};
    map_window(c, under, "lumen-test-under", 200, 150, 0xFF00FF00u);
    window over{};
    map_window(c, over, "lumen-test-over", 200, 150, 0xFFFF00FFu);
    const lx::rect2i gu = geometry_of("lumen-test-under");
    const lx::rect2i go = geometry_of("lumen-test-over");
    list_state l{};
    bind_list(c, l);
    handle_info* h = find_handle(l, "lumen-test-under");
    session s{};
    if (h)
        open_session(c, s,
                     ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
                         c.toplevel_sources, h->handle));
    check("toplevel: constraints are the window's size",
          s.width == 200 && s.height == 150 && s.dones == 1,
          std::to_string(s.width) + "x" + std::to_string(s.height));
    shm_buffer b = make_buffer(c, 200, 150, 0xFF5A5A5Au);
    frame f = capture(c, s, b, {0, 0, 200, 150});
    // Where the other window covers it on screen, the capture still shows it.
    const bool covered = go.x < gu.x + gu.width && go.y < gu.y + gu.height;
    const int cx = (go.x > gu.x ? go.x : gu.x) - gu.x + 5;
    const int cy = (go.y > gu.y ? go.y : gu.y) - gu.y + 5;
    check("toplevel: a covered window is captured with its own pixels",
          f.ready && covered && pixel(b, cx, cy) == 0x00FF00u && pixel(b, 0, 0) == 0x00FF00u,
          "covered=" + std::to_string(covered) + " px=" + hex(pixel(b, cx, cy)));

    // Its window unmaps: stopped, and a frame waiting for it fails as stopped.
    frame f2{};
    f2.frame = ext_image_copy_capture_session_v1_create_frame(s.session);
    ext_image_copy_capture_frame_v1_add_listener(f2.frame, &frame_listener, &f2);
    ext_image_copy_capture_frame_v1_attach_buffer(f2.frame, b.buffer);
    ext_image_copy_capture_frame_v1_capture(f2.frame);
    tick();
    (void)wl_display_roundtrip(c.display);
    xdg_toplevel_destroy(under.toplevel);
    xdg_surface_destroy(under.xdg);
    wl_surface_destroy(under.surface);
    (void)run_until(c, [&] { return f2.failed || f2.ready; }, 5);
    check("toplevel: the window going stops the session and fails its frame as stopped",
          s.stopped && f2.failed &&
              f2.reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED,
          "stopped=" + std::to_string(s.stopped) + " reason=" + std::to_string(f2.reason));
    xdg_toplevel_destroy(over.toplevel);
    xdg_surface_destroy(over.xdg);
    wl_surface_destroy(over.surface);
    tick();
    (void)finish(c);
}

void error_cases() {
    expect("error: create_session with an unknown option", "ext_image_copy_capture_manager_v1",
           EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION, [](client& c) {
               (void)ext_image_copy_capture_manager_v1_create_session(c.copy, output_source(c), 2);
           });
    expect("error: create_frame while a frame lives", "ext_image_copy_capture_session_v1",
           EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME, [](client& c) {
               session s{};
               open_session(c, s, output_source(c));
               (void)ext_image_copy_capture_session_v1_create_frame(s.session);
               (void)ext_image_copy_capture_session_v1_create_frame(s.session);
           });
    expect("error: capture without attach_buffer", "ext_image_copy_capture_frame_v1",
           EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER, [](client& c) {
               session s{};
               open_session(c, s, output_source(c));
               auto* f = ext_image_copy_capture_session_v1_create_frame(s.session);
               ext_image_copy_capture_frame_v1_capture(f);
           });
    expect("error: damage_buffer with a negative x", "ext_image_copy_capture_frame_v1",
           EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE, [](client& c) {
               session s{};
               open_session(c, s, output_source(c));
               auto* f = ext_image_copy_capture_session_v1_create_frame(s.session);
               ext_image_copy_capture_frame_v1_damage_buffer(f, -1, 0, 10, 10);
           });
    expect("error: damage_buffer with zero width", "ext_image_copy_capture_frame_v1",
           EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE, [](client& c) {
               session s{};
               open_session(c, s, output_source(c));
               auto* f = ext_image_copy_capture_session_v1_create_frame(s.session);
               ext_image_copy_capture_frame_v1_damage_buffer(f, 0, 0, 0, 10);
           });
    expect("error: capture twice", "ext_image_copy_capture_frame_v1",
           EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED, [](client& c) {
               session s{};
               open_session(c, s, output_source(c));
               shm_buffer b = make_buffer(c, s.width, s.height, 0);
               auto* f = ext_image_copy_capture_session_v1_create_frame(s.session);
               ext_image_copy_capture_frame_v1_attach_buffer(f, b.buffer);
               ext_image_copy_capture_frame_v1_capture(f);
               ext_image_copy_capture_frame_v1_capture(f);
           });
    expect("error: attach_buffer after capture", "ext_image_copy_capture_frame_v1",
           EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED, [](client& c) {
               session s{};
               open_session(c, s, output_source(c));
               shm_buffer b = make_buffer(c, s.width, s.height, 0);
               auto* f = ext_image_copy_capture_session_v1_create_frame(s.session);
               ext_image_copy_capture_frame_v1_attach_buffer(f, b.buffer);
               ext_image_copy_capture_frame_v1_capture(f);
               ext_image_copy_capture_frame_v1_attach_buffer(f, b.buffer);
           });
    expect("error: damage_buffer after capture", "ext_image_copy_capture_frame_v1",
           EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED, [](client& c) {
               session s{};
               open_session(c, s, output_source(c));
               shm_buffer b = make_buffer(c, s.width, s.height, 0);
               auto* f = ext_image_copy_capture_session_v1_create_frame(s.session);
               ext_image_copy_capture_frame_v1_attach_buffer(f, b.buffer);
               ext_image_copy_capture_frame_v1_capture(f);
               ext_image_copy_capture_frame_v1_damage_buffer(f, 0, 0, 4, 4);
           });
    expect("error: get_capture_session twice on a cursor session",
           "ext_image_copy_capture_cursor_session_v1",
           EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION, [](client& c) {
               wl_pointer* p = wl_seat_get_pointer(c.seat);
               auto* cs = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
                   c.copy, output_source(c), p);
               (void)ext_image_copy_capture_cursor_session_v1_get_capture_session(cs);
               (void)ext_image_copy_capture_cursor_session_v1_get_capture_session(cs);
           });
    expect("error: screencopy copy into a buffer of the wrong size", "zwlr_screencopy_frame_v1",
           ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, [](client& c) {
               auto* f = zwlr_screencopy_manager_v1_capture_output(c.screencopy, 0, c.output);
               shm_buffer b = make_buffer(c, 16, 16, 0);
               zwlr_screencopy_frame_v1_copy(f, b.buffer);
           });
    expect("error: screencopy copy twice", "zwlr_screencopy_frame_v1",
           ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED, [](client& c) {
               auto* f = zwlr_screencopy_manager_v1_capture_output(c.screencopy, 0, c.output);
               shm_buffer b = make_buffer(c, k_out_w, k_out_h, 0);
               zwlr_screencopy_frame_v1_copy(f, b.buffer);
               zwlr_screencopy_frame_v1_copy(f, b.buffer);
           });
}

// ── wlr-screencopy ──

struct sc_frame {
    zwlr_screencopy_frame_v1* frame = nullptr;
    uint32_t format = ~0u;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool buffer_done = false;
    bool ready = false;
    bool failed = false;
    std::vector<int> damage;
};

const zwlr_screencopy_frame_v1_listener sc_listener = {
    .buffer =
        [](void* d, zwlr_screencopy_frame_v1*, uint32_t format, uint32_t w, uint32_t h,
           uint32_t stride) {
            auto* f = static_cast<sc_frame*>(d);
            f->format = format;
            f->width = static_cast<int>(w);
            f->height = static_cast<int>(h);
            f->stride = static_cast<int>(stride);
        },
    .flags = [](void*, zwlr_screencopy_frame_v1*, uint32_t) {},
    .ready = [](void* d, zwlr_screencopy_frame_v1*, uint32_t, uint32_t,
                uint32_t) { static_cast<sc_frame*>(d)->ready = true; },
    .failed = [](void* d, zwlr_screencopy_frame_v1*) { static_cast<sc_frame*>(d)->failed = true; },
    .damage =
        [](void* d, zwlr_screencopy_frame_v1*, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
            static_cast<sc_frame*>(d)->damage.insert(static_cast<sc_frame*>(d)->damage.end(),
                                                     {static_cast<int>(x), static_cast<int>(y),
                                                      static_cast<int>(w), static_cast<int>(h)});
        },
    .linux_dmabuf = [](void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .buffer_done = [](void* d, zwlr_screencopy_frame_v1*) { static_cast<sc_frame*>(d)->buffer_done = true; },
};

void screencopy_cases() {
    client c{};
    if (!connect(c) || !c.screencopy) {
        check("screencopy", false, "no screencopy global");
        return;
    }
    window w{};
    map_window(c, w, "lumen-test-sc", 100, 80, 0xFF00FFFFu);
    const lx::rect2i g = geometry_of("lumen-test-sc");
    sc_frame f{};
    f.frame = zwlr_screencopy_manager_v1_capture_output(c.screencopy, 0, c.output);
    zwlr_screencopy_frame_v1_add_listener(f.frame, &sc_listener, &f);
    (void)wl_display_roundtrip(c.display);
    check("screencopy: buffer (XRGB, the output's size, its stride) and buffer_done",
          f.format == WL_SHM_FORMAT_XRGB8888 && f.width == k_out_w && f.height == k_out_h &&
              f.stride == k_out_w * 4 && f.buffer_done,
          std::to_string(f.width) + "x" + std::to_string(f.height));
    shm_buffer b = make_buffer(c, f.width, f.height, 0xFF5A5A5Au);
    zwlr_screencopy_frame_v1_copy(f.frame, b.buffer);
    (void)run_until(c, [&] { return f.ready || f.failed; });
    check("screencopy: copy is ready with the window's pixels",
          f.ready && pixel(b, g.x + 5, g.y + 5) == 0x00FFFFu, hex(pixel(b, g.x + 5, g.y + 5)));
    zwlr_screencopy_frame_v1_destroy(f.frame);

    // A region: the window's corner at the buffer's origin.
    sc_frame r{};
    r.frame = zwlr_screencopy_manager_v1_capture_output_region(c.screencopy, 0, c.output, g.x + 10,
                                                               g.y + 10, 20, 15);
    zwlr_screencopy_frame_v1_add_listener(r.frame, &sc_listener, &r);
    (void)wl_display_roundtrip(c.display);
    shm_buffer rb = make_buffer(c, r.width > 0 ? r.width : 1, r.height > 0 ? r.height : 1, 0);
    if (r.width == 20 && r.height == 15) {
        zwlr_screencopy_frame_v1_copy(r.frame, rb.buffer);
        (void)run_until(c, [&] { return r.ready || r.failed; });
    }
    check("screencopy: a region is a crop at the buffer's origin",
          r.width == 20 && r.height == 15 && r.ready && pixel(rb, 0, 0) == 0x00FFFFu &&
              pixel(rb, 19, 14) == 0x00FFFFu,
          std::to_string(r.width) + "x" + std::to_string(r.height));
    zwlr_screencopy_frame_v1_destroy(r.frame);

    // copy_with_damage: the first copy at once (full), the second waits for a change.
    sc_frame d1{};
    d1.frame = zwlr_screencopy_manager_v1_capture_output(c.screencopy, 0, c.output);
    zwlr_screencopy_frame_v1_add_listener(d1.frame, &sc_listener, &d1);
    (void)wl_display_roundtrip(c.display);
    zwlr_screencopy_frame_v1_copy_with_damage(d1.frame, b.buffer);
    (void)run_until(c, [&] { return d1.ready || d1.failed; });
    zwlr_screencopy_frame_v1_destroy(d1.frame);
    sc_frame d2{};
    d2.frame = zwlr_screencopy_manager_v1_capture_output(c.screencopy, 0, c.output);
    zwlr_screencopy_frame_v1_add_listener(d2.frame, &sc_listener, &d2);
    (void)wl_display_roundtrip(c.display);
    zwlr_screencopy_frame_v1_copy_with_damage(d2.frame, b.buffer);
    for (int i = 0; i < 4; ++i) {
        tick();
        (void)wl_display_roundtrip(c.display);
    }
    const bool waited = !d2.ready && !d2.failed;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            w.content.pixels[static_cast<std::size_t>(y) * w.content.width + x] = 0xFF000000u;
    wl_surface_attach(w.surface, w.content.buffer, 0, 0);
    wl_surface_damage_buffer(w.surface, 0, 0, 4, 4);
    wl_surface_commit(w.surface);
    (void)run_until(c, [&] { return d2.ready || d2.failed; });
    long area = 0;
    for (std::size_t i = 0; i + 3 < d2.damage.size(); i += 4)
        area += static_cast<long>(d2.damage[i + 2]) * d2.damage[i + 3];
    check("screencopy: copy_with_damage is full first, then waits for damage and reports it",
          d1.ready && d1.damage.size() == 4 && d1.damage[2] == k_out_w && waited && d2.ready &&
              area > 0 && area < 200,
          "waited=" + std::to_string(waited) + " area=" + std::to_string(area));
    zwlr_screencopy_frame_v1_destroy(d2.frame);
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    tick();
    (void)finish(c);
}

// ── the session lock ──

void lock_cases() {
    client c{};
    client locker{};
    if (!connect(c) || !connect(locker) || !locker.lock_manager) {
        check("lock", false, "no lock manager");
        return;
    }
    window w{};
    map_window(c, w, "lumen-test-lock", 120, 90, 0xFFFFFF00u);
    const lx::rect2i g = geometry_of("lumen-test-lock");
    session s{};
    open_session(c, s, output_source(c));
    shm_buffer b = make_buffer(c, s.width, s.height, 0xFF5A5A5Au);
    frame f1 = capture(c, s, b, {0, 0, s.width, s.height});
    check("lock: before the lock the window is captured",
          f1.ready && pixel(b, g.x + 5, g.y + 5) == 0xFFFF00u, hex(pixel(b, g.x + 5, g.y + 5)));

    struct lock_state {
        bool locked = false;
        bool finished = false;
    } ls;
    static const ext_session_lock_v1_listener lock_listener = {
        .locked = [](void* d, ext_session_lock_v1*) { static_cast<lock_state*>(d)->locked = true; },
        .finished = [](void* d, ext_session_lock_v1*) { static_cast<lock_state*>(d)->finished = true; },
    };
    ext_session_lock_v1* lock = ext_session_lock_manager_v1_lock(locker.lock_manager);
    ext_session_lock_v1_add_listener(lock, &lock_listener, &ls);
    (void)wl_display_roundtrip(locker.display);
    // From the request on: the next frame is the black curtain.
    frame f2 = capture(c, s, b, {});
    check("lock: from the lock request the next frame is the black curtain, full damage",
          f2.ready && pixel(b, g.x + 5, g.y + 5) == 0 && f2.damage.size() == 4 &&
              f2.damage[2] == s.width,
          hex(pixel(b, g.x + 5, g.y + 5)));
    // Then nothing until unlock.
    frame f3{};
    f3.frame = ext_image_copy_capture_session_v1_create_frame(s.session);
    ext_image_copy_capture_frame_v1_add_listener(f3.frame, &frame_listener, &f3);
    ext_image_copy_capture_frame_v1_attach_buffer(f3.frame, b.buffer);
    ext_image_copy_capture_frame_v1_capture(f3.frame);
    (void)run_until(c, [&] { return f3.ready || f3.failed; }, 8);
    check("lock: then frames are held while locked", !f3.ready && !f3.failed, "");
    (void)run_until(locker, [&] { return ls.locked; }, 10);
    if (ls.locked)
        ext_session_lock_v1_unlock_and_destroy(lock);
    else
        ext_session_lock_v1_destroy(lock);
    (void)wl_display_roundtrip(locker.display);
    (void)run_until(c, [&] { return f3.ready || f3.failed; });
    check("lock: after unlock the held frame comes, in full, with the window again",
          f3.ready && f3.damage.size() == 4 && f3.damage[2] == s.width &&
              pixel(b, g.x + 5, g.y + 5) == 0xFFFF00u,
          "locked=" + std::to_string(ls.locked) + " px=" + hex(pixel(b, g.x + 5, g.y + 5)));
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    tick();
    (void)finish(c);
    (void)finish(locker);
}

} // namespace

int main() {
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
                lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 5);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tick();

    globals_case();
    list_cases();
    output_cases();
    toplevel_cases();
    error_cases();
    screencopy_cases();
    lock_cases();

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
