// lx.app against a real compositor (ADR-010 §7 Phase 4, stage 3): lumen-hello's window
// (executables/lumen-hello/hello_ui.cppm) mapped, presented, hovered, clicked, typed into,
// resized, moved to a 1.25 output, and left idle.
//
// The compositor runs in this process (lx::compositor::compositor on a private socket, two
// virtual outputs: 1280×800 at 1 and 1280×800 at 1.25 to its right, CPU present, pinging off
// so a quiet client is not woken). The window runs in a child process — this binary with
// `--client <tier> <fd>` — because lx::runtime keeps one process-wide source table and the
// executor's UI strand, both of which the compositor's own loop already holds, and because a
// client's allocations, wakeups and polls must be its own to be counted. The child asks the
// parent, over a SOCK_SEQPACKET pair, for what only the compositor can do: virtual pointer
// motion and buttons, keys, maximise / fullscreen, a move to the other output, the toplevel
// record. The parent ticks the compositor at 60 Hz and answers.
//
// Tiers: cpu always; gl when a render node exists (forced: the headless compositor names no
// main device, so automatic picks cpu). The compositor starts with no Vulkan driver loaded so
// its CPU path keeps a bookkeeping import of GL dma-bufs (lavapipe cannot import vmwgfx
// buffers; test_client_swapchain does the same).
//
// Allocation counting: operator new and malloc on the child's UI thread while a measurement
// is open (the worker thread, libwayland's reader and the drivers' threads are not the UI's).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <png.h>

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_PROTOCOL_INPUT_METHOD_UNSTABLE_V2)
#include <wayland-client.h>
#include "input-method-unstable-v2-client-protocol.h"
#define LUMEN_TEST_IM 1
#endif

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.compositor;
import lx.scene;
import lx.ui;
import lx.app;
import lx.wayland.client;
import lumen.hello.ui;

#ifndef LUMEN_UI_GOLDEN_DIR
#define LUMEN_UI_GOLDEN_DIR "tests/golden/ui"
#endif

// ── allocation accounting (the UI thread only, while `g_counting`) ──

#if defined(__SANITIZE_ADDRESS__)
#define LUMEN_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LUMEN_TEST_SANITIZED 1
#endif
#endif

namespace {
__attribute__((tls_model("initial-exec"))) thread_local bool g_counting = false;
std::atomic<std::uint64_t> g_new_calls{0};
std::atomic<std::uint64_t> g_malloc_calls{0};
} // namespace

#if !defined(LUMEN_TEST_SANITIZED)
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void __libc_free(void*);

void* malloc(size_t n) noexcept {
    if (g_counting)
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    return __libc_malloc(n);
}
void* calloc(size_t n, size_t s) noexcept {
    if (g_counting)
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    return __libc_calloc(n, s);
}
void* realloc(void* p, size_t n) noexcept {
    if (g_counting)
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    return __libc_realloc(p, n);
}
void free(void* p) noexcept { __libc_free(p); }
}

namespace {
void* counted_new(std::size_t n) noexcept {
    if (g_counting)
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* counted_aligned_new(std::size_t n, std::align_val_t al) noexcept {
    if (g_counting)
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
    const auto a = static_cast<std::size_t>(al);
    const std::size_t rounded = (n + a - 1) / a * a;
    return std::aligned_alloc(a, rounded ? rounded : a);
}
} // namespace

void* operator new(std::size_t n) {
    if (void* p = counted_new(n))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    if (void* p = counted_new(n))
        return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = counted_aligned_new(n, a))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) {
    if (void* p = counted_aligned_new(n, a))
        return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned_new(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned_new(n, a);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

namespace {

const char* const k_socket = "lumen-test-app-window";
const char* const k_app_id = "org.lumen.hello";

[[nodiscard]] double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// ════════════════════════════════════════════════════════════════════════════════════
// The parent: the compositor and the command server
// ════════════════════════════════════════════════════════════════════════════════════

lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 0;

void tick() {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::scheduler::frame_tick t{};
    t.timestamp_ns = static_cast<long long>(now_ns());
    t.delta_seconds = 1.0 / 60.0;
    t.frame_index = static_cast<unsigned>(++g_frame);
    g_comp->tick_ui(t);
    g_comp->wayland().flush();
    (void)g_comp->render_and_wait();
    g_comp->run_posted_ui_tasks();
    g_comp->wayland().flush();
}

struct record_view {
    lx::toplevel_id id{};
    lx::rect2i geometry{};
    bool mapped = false;
    std::string title, app_id;
};

record_view find_record() {
    record_view out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, k_app_id) == 0 && r.id.id() > out.id.id()) {
            out.id = r.id;
            out.geometry = r.geometry;
            out.mapped = r.mapped;
            out.title = r.title ? r.title : "";
            out.app_id = r.app_id;
        }
    });
    return out;
}

// ── the input method: a raw libwayland client of the parent's, as fcitx5 would be ──

#if defined(LUMEN_TEST_IM)
struct ime_client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    zwp_input_method_manager_v2* manager = nullptr;
    zwp_input_method_v2* im = nullptr;
    unsigned activates = 0;
    unsigned deactivates = 0;
    unsigned dones = 0;
    std::string surrounding;
    std::uint32_t cursor = 0;
};

const zwp_input_method_v2_listener k_im_listener = {
    [](void* d, zwp_input_method_v2*) { ++static_cast<ime_client*>(d)->activates; },
    [](void* d, zwp_input_method_v2*) { ++static_cast<ime_client*>(d)->deactivates; },
    [](void* d, zwp_input_method_v2*, const char* t, uint32_t cursor, uint32_t) {
        static_cast<ime_client*>(d)->surrounding = t ? t : "";
        static_cast<ime_client*>(d)->cursor = cursor;
    },
    [](void*, zwp_input_method_v2*, uint32_t) {},
    [](void*, zwp_input_method_v2*, uint32_t, uint32_t) {},
    [](void* d, zwp_input_method_v2*) { ++static_cast<ime_client*>(d)->dones; },
    [](void*, zwp_input_method_v2*) {}};

ime_client g_ime{};

bool ime_connect(ime_client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    static const wl_registry_listener rl = {
        [](void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t) {
            auto* c = static_cast<ime_client*>(data);
            if (std::strcmp(iface, wl_seat_interface.name) == 0 && !c->seat)
                c->seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 1));
            else if (std::strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0)
                c->manager = static_cast<zwp_input_method_manager_v2*>(
                    wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1));
        },
        [](void*, wl_registry*, uint32_t) {}};
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &rl, &c);
    if (wl_display_roundtrip(c.display) < 0 || !c.seat || !c.manager)
        return false;
    c.im = zwp_input_method_manager_v2_get_input_method(c.manager, c.seat);
    zwp_input_method_v2_add_listener(c.im, &k_im_listener, &c);
    wl_display_flush(c.display);
    return true;
}

/// Reads and dispatches what the compositor sent the input method (non-blocking).
void ime_pump() {
    if (!g_ime.display)
        return;
    while (wl_display_prepare_read(g_ime.display) != 0)
        (void)wl_display_dispatch_pending(g_ime.display);
    (void)wl_display_flush(g_ime.display);
    pollfd pf{wl_display_get_fd(g_ime.display), POLLIN, 0};
    if (::poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN))
        (void)wl_display_read_events(g_ime.display);
    else
        wl_display_cancel_read(g_ime.display);
    (void)wl_display_dispatch_pending(g_ime.display);
}
#endif

void inject_move(double x, double y) {
    lx::input::pointer_motion_event m{};
    m.x = x;
    m.y = y;
    g_comp->inject_pointer_motion(m);
}

/// One command from the child, under the compositor lock; the reply.
std::string serve(const std::string& line) {
    std::istringstream in{line};
    std::string cmd;
    in >> cmd;
#if defined(LUMEN_TEST_IM)
    // The input method's view current before it answers or commits: a commit carrying a
    // stale done count is dropped by the compositor (the protocol's rule). Outside the lock:
    // the pump thread dispatches the server side of the roundtrip.
    if (cmd.rfind("ime", 0) == 0 && g_ime.display)
        (void)wl_display_roundtrip(g_ime.display);
#endif
    std::lock_guard<std::mutex> hold(*g_lock);
    const record_view rec = find_record();
    std::string reply = "ok";
    if (cmd == "record") {
        reply = std::format("ok {} {} {} {} {}|{}|{}", rec.mapped ? 1 : 0, rec.geometry.x,
                            rec.geometry.y, rec.geometry.width, rec.geometry.height, rec.title,
                            rec.app_id);
    } else if (cmd == "move" || cmd == "gmove") {
        double x = 0, y = 0;
        in >> x >> y;
        if (cmd == "move") {
            x += rec.geometry.x;
            y += rec.geometry.y;
        }
        inject_move(x, y);
    } else if (cmd == "button") {
        int p = 0;
        in >> p;
        lx::input::pointer_button_event b{};
        b.which = lx::input::button::left;
        b.evdev_code = 0x110;
        b.pressed = p != 0;
        g_comp->inject_pointer_button(b);
    } else if (cmd == "key") {
        unsigned code = 0;
        int p = 0;
        in >> code >> p;
        lx::input::key_event k{};
        k.evdev_keycode = code;
        k.pressed = p != 0;
        g_comp->inject_key(k);
    } else if (cmd == "maximize") {
        int on = 0;
        in >> on;
        g_comp->windows().set_maximized(rec.id, on != 0, "test");
    } else if (cmd == "fullscreen") {
        int on = 0;
        in >> on;
        g_comp->windows().set_fullscreen(rec.id, on != 0, -1, "test");
    } else if (cmd == "ime") {
#if defined(LUMEN_TEST_IM)
        reply = std::format("ok {} {} {} {}|{}", g_ime.im ? 1 : 0, g_ime.activates, g_ime.dones,
                            g_ime.cursor, g_ime.surrounding);
#else
        reply = "fail no input method";
#endif
    } else if (cmd == "ime_preedit" || cmd == "ime_commit") {
#if defined(LUMEN_TEST_IM)
        std::string text;
        in >> text;
        if (cmd == "ime_preedit")
            zwp_input_method_v2_set_preedit_string(g_ime.im, text.c_str(),
                                                   static_cast<int32_t>(text.size()),
                                                   static_cast<int32_t>(text.size()));
        else if (!text.empty())
            zwp_input_method_v2_commit_string(g_ime.im, text.c_str());
        zwp_input_method_v2_commit(g_ime.im, g_ime.dones);
        wl_display_flush(g_ime.display);
#else
        reply = "fail no input method";
#endif
    } else if (cmd == "output") {
        int dx = 0;
        in >> dx;
        reply = g_comp->windows().move_to_output(rec.id, dx, 0) ? "ok" : "fail";
    } else {
        reply = "fail unknown command";
    }
    g_comp->wayland().flush();
    return reply;
}

/// Runs the child `--client <tier>` to its end: ticks the compositor at 60 Hz, serves its
/// commands. Its exit status is its failure count.
int run_child(const char* tier) {
    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        std::printf("  FAIL  %s: socketpair\n", tier);
        return 1;
    }
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(sv[0]);
        const int fd = ::dup(sv[1]); // without CLOEXEC: the exec'd child keeps it
        char self[] = "/proc/self/exe";
        char flag[] = "--client";
        std::string t = tier;
        std::string f = std::to_string(fd);
        char* argv[] = {self, flag, t.data(), f.data(), nullptr};
        ::execv(self, argv);
        ::_exit(127);
    }
    ::close(sv[1]);
    int status = -1;
    double last_tick = 0.0;
    const double t0 = now_ms();
    char buf[512];
    for (;;) {
        const pid_t r = ::waitpid(child, &status, WNOHANG);
        if (r == child)
            break;
        if (now_ms() - t0 > 90'000.0) {
            std::printf("  FAIL  %s: child timed out\n", tier);
            ::kill(child, SIGKILL);
            (void)::waitpid(child, &status, 0);
            break;
        }
        pollfd pf{sv[0], POLLIN, 0};
        if (::poll(&pf, 1, 2) > 0 && (pf.revents & POLLIN)) {
            const ssize_t n = ::recv(sv[0], buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                const std::string reply = serve(buf);
                (void)::send(sv[0], reply.data(), reply.size(), MSG_NOSIGNAL);
            }
        }
        if (now_ms() - last_tick >= 16.6) {
            last_tick = now_ms();
            tick();
        }
#if defined(LUMEN_TEST_IM)
        ime_pump();
#endif
    }
    ::close(sv[0]);
    if (!WIFEXITED(status)) {
        std::printf("  FAIL  %s: child died (status %#x)\n", tier, status);
        return 1;
    }
    return WEXITSTATUS(status);
}

// ════════════════════════════════════════════════════════════════════════════════════
// The child: lumen-hello's window on lx.app
// ════════════════════════════════════════════════════════════════════════════════════

int g_failed = 0;
int g_ran = 0;
const char* g_tier = "";

void check(const char* name, bool ok, const std::string& detail) {
    ++g_ran;
    std::printf("  %s  [%s] %s — %s\n", ok ? "PASS" : "FAIL", g_tier, name, detail.c_str());
    std::fflush(stdout);
    if (!ok)
        ++g_failed;
}

void measure(const std::string& what) {
    std::printf("  MEASURE  [%s] %s\n", g_tier, what.c_str());
    std::fflush(stdout);
}

void skip(const char* name, const std::string& why) {
    std::printf("  SKIP  [%s] %s — %s\n", g_tier, name, why.c_str());
    std::fflush(stdout);
}

struct parent_link {
    int fd = -1;
    std::string call(const std::string& line) {
        (void)::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
        char buf[512];
        const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            return "fail";
        buf[n] = '\0';
        return buf;
    }
};

struct record_reply {
    bool ok = false;
    bool mapped = false;
    lx::rect2i geometry{};
    std::string title, app_id;
};

record_reply query_record(parent_link& p) {
    record_reply out{};
    const std::string r = p.call("record");
    if (r.rfind("ok ", 0) != 0)
        return out;
    int mapped = 0;
    std::istringstream in{r.substr(3)};
    in >> mapped >> out.geometry.x >> out.geometry.y >> out.geometry.width >> out.geometry.height;
    const std::size_t bar = r.find('|');
    const std::size_t bar2 = r.find('|', bar + 1);
    if (bar != std::string::npos && bar2 != std::string::npos) {
        out.title = r.substr(bar + 1, bar2 - bar - 1);
        out.app_id = r.substr(bar2 + 1);
    }
    out.mapped = mapped != 0;
    out.ok = true;
    return out;
}

[[nodiscard]] lx::ui::ui_node* find_key(lx::ui::ui_node* n, lx::ui::element_key key) {
    if (!n)
        return nullptr;
    if (n->key() == key)
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (lx::ui::ui_node* hit = find_key(n->child_at(i), key))
            return hit;
    return nullptr;
}

[[nodiscard]] lx::ui::ui_node* find_type(lx::ui::ui_node* n, lx::ui::element_type_id t) {
    if (!n)
        return nullptr;
    if (n->type() == t)
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (lx::ui::ui_node* hit = find_type(n->child_at(i), t))
            return hit;
    return nullptr;
}

[[nodiscard]] lx::point2i center(lx::rect2i r) { return {r.x + r.width / 2, r.y + r.height / 2}; }

/// Runs the loop for `ms` with nothing but one quit timer: what an idle window does.
void run_for(lx::application& app, int ms) {
    lx::runtime::event_loop& loop = app.event_loop();
    lx::runtime::timer_source quit{0, lx::runtime::task{[&loop] { loop.quit(0); }}, false};
    quit.schedule_at(loop.now() + static_cast<long long>(ms) * 1'000'000);
    loop.add_source(&quit);
    (void)loop.run();
    loop.remove_source(&quit);
}

/// Until the window has drawn what it owes and its last frame's callback came back.
bool settle(lx::application& app, lx::app::window& w, int timeout_ms = 3000) {
    return app.run_until(
        [&] {
            return !w.frame_due() && !w.surface().frame_pending() && !w.root().wants_frame() &&
                   !w.input().motion_pending();
        },
        timeout_ms);
}

[[nodiscard]] bool rect_meets(lx::rect2i a, lx::rect2i b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

/// The window's CPU image against a golden PNG (written when LUMEN_UI_GOLDEN_UPDATE=1).
struct golden_result {
    bool compared = false;
    bool written = false;
    int max_error = 0;
    unsigned differing = 0;
    std::string note;
};

golden_result compare_golden(const lx::gfx::pixel_surface& px, const char* name) {
    golden_result out{};
    const std::string dir = std::getenv("LUMEN_UI_GOLDEN_DIR") ? std::getenv("LUMEN_UI_GOLDEN_DIR")
                                                               : LUMEN_UI_GOLDEN_DIR;
    const std::string path = dir + "/" + name + ".png";
    const unsigned w = px.width;
    const unsigned h = px.height;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
    for (unsigned y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(px.pixels + static_cast<std::size_t>(y) * px.stride);
        for (unsigned x = 0; x < w; ++x) {
            const std::uint32_t v = row[x];
            std::uint8_t* o = rgb.data() + (static_cast<std::size_t>(y) * w + x) * 3;
            o[0] = static_cast<std::uint8_t>(v >> 16);
            o[1] = static_cast<std::uint8_t>(v >> 8);
            o[2] = static_cast<std::uint8_t>(v);
        }
    }
    const char* update = std::getenv("LUMEN_UI_GOLDEN_UPDATE");
    if (update && update[0] == '1') {
        png_image img{};
        img.version = PNG_IMAGE_VERSION;
        img.width = w;
        img.height = h;
        img.format = PNG_FORMAT_RGB;
        out.written = png_image_write_to_file(&img, path.c_str(), 0, rgb.data(), 0, nullptr) != 0;
        out.note = out.written ? "reference written" : "write failed";
        return out;
    }
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path.c_str())) {
        out.note = "no reference " + path;
        return out;
    }
    img.format = PNG_FORMAT_RGB;
    std::vector<std::uint8_t> ref(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, nullptr, ref.data(), 0, nullptr) || img.width != w ||
        img.height != h) {
        out.note = std::format("reference {}×{} vs image {}×{}", img.width, img.height, w, h);
        return out;
    }
    out.compared = true;
    for (std::size_t i = 0; i < rgb.size(); i += 3) {
        int e = 0;
        for (unsigned c = 0; c < 3; ++c)
            e = std::max(e, std::abs(int(rgb[i + c]) - int(ref[i + c])));
        out.max_error = std::max(out.max_error, e);
        out.differing += e > 0 ? 1u : 0u;
    }
    return out;
}

/// Every sprite of `d` lands on whole device pixels and samples its tile 1:1 (text and
/// icons rasterised at the device scale, not resampled): the "not blurred" property.
struct sprite_check {
    unsigned sprites = 0;
    unsigned off_grid = 0;
    unsigned resampled = 0;
};

sprite_check check_sprites(const lx::scene::draw_list& d) {
    sprite_check out{};
    for (const lx::gfx::sprite_instance& s : d.sprite_instances()) {
        ++out.sprites;
        for (float v : s.bounds)
            if (v != std::floor(v))
                ++out.off_grid;
        if (static_cast<float>(s.uv[2] - s.uv[0]) != s.bounds[2] ||
            static_cast<float>(s.uv[3] - s.uv[1]) != s.bounds[3])
            ++out.resampled;
    }
    return out;
}

int client_main(const char* tier_name, int fd) {
    g_tier = tier_name;
    ::unsetenv("LUMEN_UI_BACKEND");
    parent_link parent{fd};
    lx::application app{0, nullptr};
    app.config().socket = k_socket;
    app.config().backend = lx::gfx::parse_client_tier(tier_name);
    app.config().watch_theme = 0;
    app.config().theme = "one-dark";
    if (auto s = app.start(); !s) {
        check("connect", false, lx::format_error(s.get_error()));
        return g_failed;
    }
    lx::app::window_options o{};
    o.title = "Hello Lumen";
    o.app_id = k_app_id;
    o.size = {640, 480};
    o.describe = &hello::describe;
    lx::app::window w;
    if (auto opened = w.open(app, o); !opened) {
        check("window opens", false, lx::format_error(opened.get_error()));
        return g_failed;
    }
    const lx::gfx::client_tier want = lx::gfx::parse_client_tier(tier_name);

    // ── map: first frame presented at its size, the toplevel record ──
    {
        const bool presented = app.run_until([&] { return w.timeline().first_presented_ns != 0; }, 5000);
        const record_reply rec = query_record(parent);
        const lx::app::window_timeline& t = w.timeline();
        const lx::app::app_timeline& at = app.timeline();
        check("map: the tier asked for", w.tier() == want,
              std::format("tier {} ({})", lx::gfx::client_tier_name(w.tier()), w.gpu().name()));
        check("map: first frame presented at 640×480, scale 1",
              presented && w.buffer_size().width == 640 && w.buffer_size().height == 480 &&
                  w.scale() == 1.f && rec.mapped && rec.geometry.width == 640 &&
                  rec.geometry.height == 480,
              std::format("presented {} buffer {}×{} scale {} record {}×{} mapped {}", presented,
                          w.buffer_size().width, w.buffer_size().height, w.scale(),
                          rec.geometry.width, rec.geometry.height, rec.mapped));
        check("map: the toplevel record has the title and app_id",
              rec.title == "Hello Lumen" && rec.app_id == k_app_id,
              std::format("title '{}' app_id '{}'", rec.title, rec.app_id));
        measure(std::format("start-up (child process, connect from construction): connect {:.2f} ms, "
                            "first configure {:.2f} ms, first commit {:.2f} ms, first presented "
                            "{:.2f} ms after construction",
                            (at.connect_ns - at.exec_ns) / 1e6, (t.first_configure_ns - at.exec_ns) / 1e6,
                            (t.first_commit_ns - at.exec_ns) / 1e6,
                            (t.first_presented_ns - at.exec_ns) / 1e6));
    }
    // One window per application: a second open is refused, and the first keeps its input
    // (everything below runs on it).
    {
        lx::app::window second;
        lx::app::window_options o2 = o;
        o2.title = "second";
        auto refused = second.open(app, o2);
        check("one window per application: a second open() is refused (invalid_argument)",
              !refused && refused.get_error().domain == lx::error_domain::invalid_argument &&
                  !second.is_open() && w.is_open(),
              refused ? "opened" : lx::format_error(refused.get_error()));
    }
    // The pointer off the window (output 0's corner), the UI settled.
    (void)parent.call("gmove 4 4");
    (void)settle(app, w);

    lx::ui::ui_root& root = w.root();
    const auto bounds_of = [&](lx::ui::element_key k) {
        lx::ui::ui_node* n = find_key(root.tree(), k);
        return n ? n->bounds() : lx::rect2i{};
    };

    // ── fractional scale: the window moved to the 1.25 output ──
    if (want == lx::gfx::client_tier::cpu || want == lx::gfx::client_tier::gl) {
        // The checks wait for the new scale's frame to be presented: feedback per frame here
        // (the default asks only for the first frame's).
        w.set_presentation_feedback(true);
        const std::uint64_t realloc0 = w.stats().reallocations;
        const std::uint64_t presented0 = w.stats().presented;
        const std::string moved = parent.call("output 1");
        const bool scaled = app.run_until(
            [&] {
                return w.scale() == 1.25f && w.stats().presented > presented0 &&
                       !w.surface().frame_pending();
            },
            5000);
        const record_reply rec = query_record(parent);
        check("fractional 1.25: buffer ceil(640×1.25)×ceil(480×1.25) = 800×600, viewport 640×480",
              moved == "ok" && scaled && w.buffer_size().width == 800 && w.buffer_size().height == 600 &&
                  w.viewport_destination().width == 640 && w.viewport_destination().height == 480 &&
                  w.buffer_scale() == 1 && rec.geometry.width == 640 && rec.geometry.height == 480,
              std::format("{} scale {} buffer {}×{} viewport {}×{} buffer_scale {} record {}×{}",
                          moved, w.scale(), w.buffer_size().width, w.buffer_size().height,
                          w.viewport_destination().width, w.viewport_destination().height,
                          w.buffer_scale(), rec.geometry.width, rec.geometry.height));
        check("fractional 1.25: one swapchain reallocation for the new size",
              w.stats().reallocations == realloc0 + 1,
              std::format("{} reallocation(s)", w.stats().reallocations - realloc0));
        const sprite_check sc = check_sprites(w.last_draws());
        check("fractional 1.25: text and icons not blurred (every sprite on whole device pixels, 1:1 texels)",
              sc.sprites > 20 && sc.off_grid == 0 && sc.resampled == 0,
              std::format("{} sprites, {} off the pixel grid, {} resampled", sc.sprites, sc.off_grid,
                          sc.resampled));
        if (want == lx::gfx::client_tier::cpu) {
            if (const lx::gfx::pixel_surface* px = w.cpu_pixels()) {
                const golden_result g = compare_golden(*px, "app_hello_1_25");
                if (g.written)
                    check("fractional 1.25: golden image written", true, g.note);
                else
                    check("fractional 1.25: the window's pixels match the golden (cpu, exact)",
                          g.compared && g.max_error == 0,
                          g.compared ? std::format("max error {}, {} pixels differ", g.max_error, g.differing)
                                     : g.note);
            } else {
                check("fractional 1.25: cpu pixels readable", false, "no image");
            }
        }
        const std::uint64_t presented1 = w.stats().presented;
        const std::string back = parent.call("output -1");
        const bool unscaled = app.run_until(
            [&] { return w.scale() == 1.f && w.stats().presented > presented1; }, 5000);
        check("back on the 1× output: 640×480 again", back == "ok" && unscaled &&
                  w.buffer_size().width == 640 && w.buffer_size().height == 480,
              std::format("{} scale {} buffer {}×{}", back, w.scale(), w.buffer_size().width,
                          w.buffer_size().height));
        (void)parent.call("gmove 4 4");
        (void)settle(app, w);
        w.set_presentation_feedback(false);
    }

    const lx::rect2i button = bounds_of(hello::k_button);
    const lx::rect2i counter = bounds_of(hello::k_counter);
    const lx::rect2i checkbox = bounds_of(hello::k_checkbox);
    lx::ui::ui_node* field_node = find_type(root.tree(), lx::ui::text_input_node::static_type());
    auto* field = static_cast<lx::ui::text_input_node*>(field_node);
    lx::ui::ui_node* button_node = find_key(root.tree(), hello::k_button);
    auto* form = static_cast<hello::form_host*>(find_key(root.tree(), hello::k_form));
    if (!button_node || !field || !form || button.width <= 0) {
        check("the hello tree has its button, counter, field and form", false, "missing");
        return g_failed;
    }

    // ── hover: one frame, ≤ 2 damage rects (reduced motion: the state change alone) ──
    {
        lx::ui::set_reduced_motion(true);
        const lx::app::window_stats s0 = w.stats();
        const lx::app::input_stats i0 = w.input().stats();
        const lx::point2i c = center(button);
        (void)parent.call(std::format("move {} {}", c.x, c.y));
        (void)app.run_until([&] { return w.stats().frames > s0.frames; }, 2000);
        const lx::app::window_stats s1 = w.stats();
        const lx::gfx::damage_region dmg = w.last_damage();
        // Well before the tooltip's 500 ms hover intent.
        run_for(app, 150);
        const lx::app::window_stats s2 = w.stats();
        const bool hovered = button_node->has_flag(lx::ui::node_flag::hovered);
        check("hover: the button takes the hover; one frame, damage ≤ 2 rects inside the button",
              hovered && s1.frames == s0.frames + 1 && dmg.count >= 1 && dmg.count <= 2 &&
                  rect_meets(dmg.bounds(), button),
              std::format("hovered {} frames {} damage {} rect(s) bounds {},{} {}×{}", hovered,
                          s1.frames - s0.frames, dmg.count, dmg.bounds().x, dmg.bounds().y,
                          dmg.bounds().width, dmg.bounds().height));
        check("hover: exactly one frame requested, none after it while nothing changes",
              s2.frame_requests == s0.frame_requests + 1 && s2.frames == s1.frames,
              std::format("{} request(s), {} frame(s) in the 150 ms after",
                          s2.frame_requests - s0.frame_requests, s2.frames - s1.frames));
        check("hover: the pointer cursor over the button (wp_cursor_shape_v1)",
              w.input().cursor() == lx::ui::cursor_style::pointer &&
                  w.input().stats().cursor_shapes > i0.cursor_shapes,
              std::format("cursor {} shapes sent {}", static_cast<int>(w.input().cursor()),
                          w.input().stats().cursor_shapes - i0.cursor_shapes));
        lx::ui::set_reduced_motion(false);
    }

    // ── click: the reducer dispatches, the counter label repaints in one rect ──
    {
        lx::ui::set_reduced_motion(true);
        (void)settle(app, w);
        (void)parent.call("button 1");
        (void)settle(app, w);
        const std::uint64_t f0 = w.stats().frames;
        (void)parent.call("button 0");
        const bool counted = app.run_until(
            [&] { return form->state().clicks == 1 && w.stats().frames > f0 && !w.root().wants_frame(); },
            2000);
        const lx::gfx::damage_region dmg = w.last_damage();
        unsigned over_label = 0;
        for (unsigned i = 0; i < dmg.count; ++i)
            if (rect_meets(dmg.rects[i], counter))
                ++over_label;
        check("click: the reducer counts it and the label repaints with one damage rect",
              counted && form->state().clicks == 1 && over_label == 1 && dmg.count <= 2,
              std::format("clicks {} frame rects {} ({} over the label) frames {}",
                          form->state().clicks, dmg.count, over_label, w.stats().frames - f0));
        lx::ui::set_reduced_motion(false);
    }

    // ── typing into the field (the compositor's virtual keyboard) ──
    {
        const lx::point2i c = center(field->bounds());
        (void)parent.call(std::format("move {} {}", c.x, c.y));
        (void)parent.call("button 1");
        (void)parent.call("button 0");
        (void)app.run_until([&] { return field->has_flag(lx::ui::node_flag::focused); }, 2000);
        check("field: a click focuses it, the I-beam over it",
              field->has_flag(lx::ui::node_flag::focused) &&
                  w.input().cursor() == lx::ui::cursor_style::text,
              std::format("focused {} cursor {}", field->has_flag(lx::ui::node_flag::focused),
                          static_cast<int>(w.input().cursor())));
        const std::uint64_t ime0 = w.input().stats().ime_commits;
        // KEY_H 35, KEY_I 23.
        for (unsigned code : {35u, 23u}) {
            (void)parent.call(std::format("key {} 1", code));
            (void)parent.call(std::format("key {} 0", code));
        }
        const bool typed = app.run_until([&] { return field->text() == "hi"; }, 2000);
        check("field: typing inserts the text", typed, std::format("text '{}'", field->text()));
        const auto ime_state = [&] {
            struct st {
                bool ok = false;
                unsigned activates = 0, dones = 0, cursor = 0;
                std::string surrounding;
            } out{};
            const std::string r = parent.call("ime");
            if (r.rfind("ok ", 0) != 0)
                return out;
            int up = 0;
            std::istringstream in{r.substr(3)};
            in >> up >> out.activates >> out.dones >> out.cursor;
            const std::size_t bar = r.find('|');
            out.surrounding = bar == std::string::npos ? "" : r.substr(bar + 1);
            out.ok = up != 0;
            return out;
        };
        if (!w.input().text_input().valid()) {
            skip("text-input-v3", "no zwp_text_input_manager_v3");
        } else if (!ime_state().ok) {
            skip("text-input-v3", "no input method in the parent");
        } else {
            run_for(app, 50);
            const std::uint64_t ime = w.input().stats().ime_commits - ime0;
            const auto im = ime_state();
            check("text-input-v3: the focused field enables it; its text and caret reach the input "
                  "method, one commit per edit",
                  w.input().text_input_enabled() && im.activates >= 1 && im.surrounding == "hi" &&
                      im.cursor == 2 && ime >= 1 && ime <= 3,
                  std::format("enabled {} activates {} surrounding '{}' cursor {} commits {} for 2 edits",
                              w.input().text_input_enabled(), im.activates, im.surrounding, im.cursor, ime));
            (void)parent.call("ime_preedit \xce\xb1"); // α
            const bool pre = app.run_until([&] { return field->preedit() == "\xce\xb1"; }, 2000);
            check("text-input-v3: a preedit shows at the caret, not in the text",
                  pre && field->text() == "hi",
                  std::format("preedit '{}' text '{}'", field->preedit(), field->text()));
            (void)parent.call("ime_commit ok");
            const bool committed = app.run_until([&] { return field->text() == "hiok"; }, 2000);
            run_for(app, 50);
            // The input method answers what it was sent (as fcitx5 and ibus do): a done with
            // the client's serial, after which a state held back by a stale done goes out.
            (void)parent.call("ime_commit");
            run_for(app, 50);
            const auto after = ime_state();
            check("text-input-v3: commit_string inserts at the caret, the preedit clears, the new "
                  "state reaches the input method (held past a stale done until one matches)",
                  committed && field->preedit().empty() && after.surrounding == "hiok" && after.cursor == 4,
                  std::format("text '{}' preedit '{}' input method sees '{}' cursor {} ({} dones "
                              "applied, {} stale, {} commits)",
                              field->text(), field->preedit(), after.surrounding, after.cursor,
                              w.input().stats().ime_applied, w.input().stats().ime_stale,
                              w.input().stats().ime_commits));
        }

        // Key repeat: hold KEY_A (30) past the delay.
        const std::size_t before = field->text().size();
        (void)parent.call("key 30 1");
        (void)app.run_until([&] { return w.input().repeat_deadline_ns() != 0; }, 1000);
        const bool armed = w.input().repeat_deadline_ns() != 0 && app.armed_deadline_ns() != 0;
        run_for(app, 900);
        const std::size_t held = field->text().size();
        const std::uint64_t repeats = w.input().stats().repeats;
        (void)parent.call("key 30 0");
        (void)app.run_until([&] { return w.input().repeat_deadline_ns() == 0; }, 1000);
        const std::size_t released = field->text().size();
        run_for(app, 300);
        const std::size_t after = field->text().size();
        check("key repeat: the timer arms while the key is held and repeats it",
              armed && held >= before + 3 && repeats >= 2,
              std::format("armed {} text {}→{} ({} repeats in 900 ms)", armed, before, held, repeats));
        check("key repeat: the timer disarms on release (no repeat after it)",
              w.input().repeat_deadline_ns() == 0 && after == released,
              std::format("repeat deadline {} text {} → {} in the 300 ms after",
                          w.input().repeat_deadline_ns(), released, after));
    }

    // ── idle (G1): 2 s after the last frame — nothing requested, committed, armed or woken ──
    {
        // The focus off the field (its caret blinks while focused) onto the button by Tab,
        // the pointer off the window (no tooltip delay running).
        (void)parent.call("key 15 1"); // KEY_TAB
        (void)parent.call("key 15 0");
        (void)parent.call("gmove 4 4");
        (void)settle(app, w);
        (void)app.run_until([&] { return app.armed_deadline_ns() == 0 && !w.root().wants_frame(); }, 3000);
        run_for(app, 100);
        const lx::app::window_stats s0 = w.stats();
        const lx::app::app_stats a0 = app.stats();
        const unsigned long long wake0 = app.event_loop().wakeups();
        const unsigned long long reads0 = app.display_reads();
        run_for(app, 2000);
        const lx::app::window_stats s1 = w.stats();
        const lx::app::app_stats a1 = app.stats();
        const unsigned long long wakeups = app.event_loop().wakeups() - wake0;
        check("idle 2 s: 0 frame callbacks requested, 0 commits, 0 timer expirations, the loop asleep",
              s1.frame_requests == s0.frame_requests && s1.commits == s0.commits &&
                  a1.timer_expirations == a0.timer_expirations && app.armed_deadline_ns() == 0 &&
                  wakeups <= 1 && app.display_reads() == reads0,
              std::format("{} requests, {} commits, {} expirations, timer {}, {} wakeup(s) (the "
                          "test's own quit timer is 1), {} socket reads",
                          s1.frame_requests - s0.frame_requests, s1.commits - s0.commits,
                          a1.timer_expirations - a0.timer_expirations,
                          app.armed_deadline_ns() ? "armed" : "disarmed", wakeups,
                          app.display_reads() - reads0));
    }

    // ── resize: maximise, fullscreen, restore — one reallocation per size, frame-perfect ──
    {
        struct step {
            const char* cmd;
            const char* what;
        };
        const step steps[] = {{"maximize 1", "maximized"},
                              {"fullscreen 1", "fullscreen"},
                              {"fullscreen 0", "back from fullscreen"},
                              {"maximize 0", "restored"}};
        lx::size2i last = w.buffer_size();
        for (const step& st : steps) {
            const std::uint64_t realloc0 = w.stats().reallocations;
            const std::uint64_t acks0 = w.stats().acks;
            const std::uint64_t frames0 = w.stats().frames;
            (void)parent.call(st.cmd);
            const bool drawn = app.run_until(
                [&] { return w.stats().acks > acks0 && w.stats().frames > frames0 && !w.frame_due(); }, 3000);
            run_for(app, 60); // the compositor applies the commit at its next tick
            const record_reply rec = query_record(parent);
            const lx::size2i now = w.buffer_size();
            const bool new_size = now.width != last.width || now.height != last.height;
            const std::uint64_t reallocs = w.stats().reallocations - realloc0;
            check(std::format("resize ({}): the commit after the ack has the new size; {} reallocation",
                              st.what, new_size ? "one" : "no")
                      .c_str(),
                  drawn && rec.geometry.width == w.logical_size().width &&
                      rec.geometry.height == w.logical_size().height &&
                      reallocs == (new_size ? 1u : 0u),
                  std::format("buffer {}×{} record {}×{} reallocations {}", now.width, now.height,
                              rec.geometry.width, rec.geometry.height, reallocs));
            last = now;
        }
    }

    // ── G5: 0 allocations per steady frame — the pointer back and forth after a warm-up ──
    {
        (void)settle(app, w);
        const lx::point2i a = center(button);
        const lx::point2i b = center(checkbox);
        const auto sweep = [&](unsigned n, bool count) {
            char line[64];
            for (unsigned i = 0; i < n; ++i) {
                const lx::point2i p = (i % 2) ? b : a;
                // The test's own request to the parent is not the window's: not counted.
                g_counting = false;
                const int len = std::snprintf(line, sizeof line, "move %d %d", p.x, p.y);
                (void)::send(parent.fd, line, static_cast<std::size_t>(len), MSG_NOSIGNAL);
                (void)::recv(parent.fd, line, sizeof line, 0);
                g_counting = count;
                // ~80 ms each: the hover fade runs (≈ 9 frames) and no tooltip opens.
                run_for(app, 80);
            }
            g_counting = false;
        };
        sweep(6, false); // warm-up: every state of both controls drawn once
        (void)settle(app, w);
        const std::uint64_t frames0 = w.stats().frames;
        const std::uint64_t new0 = g_new_calls.load();
        const std::uint64_t malloc0 = g_malloc_calls.load();
        sweep(10, true);
        g_counting = true;
        run_for(app, 100); // the last fade's tail
        g_counting = false;
        const std::uint64_t frames = w.stats().frames - frames0;
        const std::uint64_t news = g_new_calls.load() - new0;
        const std::uint64_t mallocs = g_malloc_calls.load() - malloc0;
#if !defined(LUMEN_TEST_SANITIZED)
        check("G5: 0 operator new over steady hover frames (event, frame loop, tick, render, commit)",
              frames >= 10 && news == 0,
              std::format("{} frames, {} operator new, {} malloc ({:.1f} per frame: libwayland's "
                          "closures and proxies{})",
                          frames, news, mallocs, frames ? double(mallocs) / frames : 0.0,
                          want == lx::gfx::client_tier::gl ? ", Mesa" : ""));
#else
        skip("G5 allocation count", "sanitizer build");
#endif
        // Frame cost from the ring: the UI thread from the event or callback to the commit.
        std::vector<double> us;
        const unsigned n = w.frame_count();
        for (unsigned i = n > frames ? n - static_cast<unsigned>(frames) : 0; i < n; ++i) {
            const lx::app::frame_record& r = w.frame_at(i);
            if (r.start_ns && r.commit_ns >= r.start_ns)
                us.push_back((r.commit_ns - r.start_ns) / 1000.0);
        }
        std::sort(us.begin(), us.end());
        if (!us.empty())
            measure(std::format("hover frame of the sweep, UI thread (frame start → commit, {} frames): p50 {:.0f} µs, "
                                "p95 {:.0f} µs; input: {} motions, {} coalesced, {} hit tests",
                                us.size(), us[us.size() / 2], us[us.size() * 95 / 100],
                                w.input().stats().motions, w.input().stats().motions_coalesced,
                                w.input().stats().hit_tests));
        // wp_presentation: by default only until the first frame is presented — the sweep's
        // frames asked for none; asked for, every frame is recorded with its presentation.
        unsigned sweep_presented = 0;
        for (unsigned i = n > frames ? n - static_cast<unsigned>(frames) : 0; i < n; ++i)
            sweep_presented += w.frame_at(i).presented ? 1u : 0u;
        check("wp_presentation: off by default after the first presented frame (no feedback "
              "for the sweep's frames)",
              frames >= 10 && sweep_presented == 0,
              std::format("{} of {} sweep frames carry a presentation", sweep_presented, frames));
        w.set_presentation_feedback(true);
        const std::uint64_t p0 = w.stats().presented;
        const std::uint64_t f0 = w.stats().frames;
        for (unsigned i = 0; i < 4; ++i) {
            const lx::point2i p = (i % 2) ? b : a;
            (void)parent.call(std::format("move {} {}", p.x, p.y));
            run_for(app, 80);
        }
        (void)settle(app, w);
        run_for(app, 100);
        unsigned presented = 0;
        for (unsigned i = 0; i < w.frame_count(); ++i)
            presented += w.frame_at(i).presented ? 1u : 0u;
        check("wp_presentation: asked for, committed frames are recorded with their presentation",
              w.stats().frames > f0 && w.stats().presented > p0 && presented > 0,
              std::format("{} frames, {} presented ({} in the ring), {} discarded",
                          w.stats().frames - f0, w.stats().presented - p0, presented,
                          w.stats().discarded));
        w.set_presentation_feedback(false);
    }

    w.close();
    // Closed: the seat names the destroyed surface nowhere (events in flight for it arrive
    // with no target and are not routed into a window reopened on the same object).
    if (lx::wayland::seat* seat = app.display().default_seat())
        check("close: the seat forgets the window's surface (pointer and keyboard focus)",
              seat->pointer_focus() == nullptr && seat->keyboard_focus() == nullptr,
              std::format("pointer focus {} keyboard focus {}", seat->pointer_focus() ? "set" : "none",
                          seat->keyboard_focus() ? "set" : "none"));

    // ── min_size against the configure's states (xdg-shell): the same window object
    // reopened with a minimum larger than the output ──
    {
        lx::app::window_options om = o;
        om.min_size = {1400, 900};
        if (auto opened = w.open(app, om); !opened) {
            check("min_size: the window reopens", false, lx::format_error(opened.get_error()));
        } else {
            const bool presented =
                app.run_until([&] { return w.timeline().first_presented_ns != 0; }, 5000);
            const lx::size2i free_size = w.logical_size();
            check("min_size: a size the client picks is at least the minimum",
                  presented && free_size.width >= 1400 && free_size.height >= 900,
                  std::format("{}×{}", free_size.width, free_size.height));
            struct step {
                const char* cmd;
                const char* what;
                bool exact; ///< maximized: the size is the size; fullscreen: at most it
            };
            const step steps[] = {{"maximize 1", "maximized: the configured size, below the minimum", true},
                                  {"fullscreen 1", "fullscreen: within the configured size", false}};
            for (const step& st : steps) {
                const std::uint64_t acks0 = w.stats().acks;
                const std::uint64_t frames0 = w.stats().frames;
                (void)parent.call(st.cmd);
                const bool drawn = app.run_until(
                    [&] { return w.stats().acks > acks0 && w.stats().frames > frames0 && !w.frame_due(); },
                    3000);
                run_for(app, 60);
                const lx::size2i cfg = w.toplevel().last_configure().size;
                const lx::size2i now = w.logical_size();
                const record_reply rec = query_record(parent);
                const bool fits = st.exact ? now.width == cfg.width && now.height == cfg.height
                                           : now.width <= cfg.width && now.height <= cfg.height;
                check(std::format("min_size {}", st.what).c_str(),
                      drawn && cfg.width > 0 && cfg.height > 0 && fits && cfg.width < 1400 &&
                          rec.geometry.width == now.width && rec.geometry.height == now.height,
                      std::format("configured {}×{} window {}×{} record {}×{}", cfg.width,
                                  cfg.height, now.width, now.height, rec.geometry.width,
                                  rec.geometry.height));
            }
            (void)parent.call("fullscreen 0");
            (void)settle(app, w);
            const std::uint64_t acks0 = w.stats().acks;
            (void)parent.call("maximize 0");
            (void)app.run_until([&] { return w.stats().acks > acks0 && !w.frame_due(); }, 3000);
            run_for(app, 60);
            const lx::size2i back = w.logical_size();
            check("min_size: restored (no bounding state), the minimum holds again",
                  back.width >= 1400 && back.height >= 900,
                  std::format("{}×{} (configured {}×{}, states {:#x})", back.width, back.height,
                              w.toplevel().last_configure().size.width,
                              w.toplevel().last_configure().size.height,
                              w.toplevel().last_configure().states));
            w.close();
        }
    }
    std::printf("  [%s] %d checks, %d failed\n", g_tier, g_ran, g_failed);
    return g_failed;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--client") == 0)
        return client_main(argv[2], std::atoi(argv[3]));

    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 60.0;
    cfg.present = lx::compositor::present_backend::cpu;
    cfg.virtual_outputs = "1280x800,1280x800@1.25";
    cfg.ping_timeout_ms = 0;
    lx::compositor::compositor comp{cfg};
    // No Vulkan driver in the compositor: its CPU path keeps a bookkeeping import of the GL
    // tier's dma-bufs (lavapipe cannot import vmwgfx buffers). The child loads its own.
    ::setenv("VK_LOADER_DRIVERS_DISABLE", "*", 1);
    auto started = comp.start();
    ::unsetenv("VK_LOADER_DRIVERS_DISABLE");
    if (!started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    std::atomic<bool> pump_on{true};
    std::mutex dispatch_lock;
    g_comp = &comp;
    g_lock = &dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int loop_fd = comp.wayland().event_fd();
        while (pump_on.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 2);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tick();
#if defined(LUMEN_TEST_IM)
    if (!ime_connect(g_ime))
        std::printf("  SKIP  no zwp_input_method_manager_v2: text-input-v3 is not driven\n");
#endif

    int failed = 0;
    std::printf("tier cpu:\n");
    failed += run_child("cpu");
    struct stat st{};
    if (::stat("/dev/dri/renderD128", &st) == 0) {
        std::printf("tier gl (forced; the headless compositor names no main device):\n");
        failed += run_child("gl");
    } else {
        std::printf("  SKIP  [gl] no render node\n");
    }

    pump_on.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d failed\n", failed);
    return failed == 0 ? 0 : 1;
}
