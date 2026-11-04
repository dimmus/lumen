// lx.ui.host against a real compositor (ADR-010 §7 Phase 5): one context driving surfaces of
// every role — two toplevels, a layer-shell panel, a popup on the panel, an alpha subsurface —
// on the cpu tier and, when a render node exists, the gl tier.
//
// The compositor runs in this process (lx::compositor::compositor on a private socket, one
// virtual 1280×800 output at scale 1, CPU present, pinging off). The client runs in a child
// process — this binary with `--client <tier> <fd>` — as in test_app_window: lx::runtime keeps
// one process-wide source table and UI strand, which the compositor's loop already holds, and
// a client's allocations must be its own to be counted. The child asks the parent, over a
// SOCK_SEQPACKET pair, for what only the compositor can do: pointer motion and buttons, keys,
// a toplevel's record, the output's usable area. The parent ticks the compositor at 60 Hz.
//
// The child drives lx.ui.host directly, as the shell will: its own display, event loop,
// display source and context — no lx.app.
//
// The gl tier is forced (the headless compositor names no main device). The compositor keeps
// no Vulkan driver loaded, so its CPU path holds a bookkeeping import of GL dma-bufs and the
// gl tier's pixels cannot be read back here: its "no corruption" check is that 300 frames of
// two surfaces drawn in the same loop iterations fail nothing, and the ring's slot waits are
// recorded. The cpu tier's pixels are read and compared.
//
// Allocation counting: operator new and malloc on the child's UI thread while a measurement
// is open.
#include "render_node.hpp"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.layout;
import lx.compositor;
import lx.scene;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;

// ── allocation accounting (the UI thread only, while `g_counting`) ──

#ifdef __SANITIZE_ADDRESS__
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

#ifndef LUMEN_TEST_SANITIZED
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

namespace host = lx::ui::host;

const char* const k_socket = "lumen-test-ui-host";
const char* const k_app_a = "org.lumen.test.host.a";
const char* const k_app_b = "org.lumen.test.host.b";
const char* const k_app_c = "org.lumen.test.host.c";
const char* const k_app_other = "org.lumen.test.host.other";

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

/// One command from the child, under the compositor lock; the reply.
std::string serve(const std::string& line) {
    std::istringstream in{line};
    std::string cmd;
    in >> cmd;
    std::lock_guard<std::mutex> hold(*g_lock);
    std::string reply = "ok";
    if (cmd == "record") {
        // `record APP_ID`: "ok <mapped> x y w h" of the newest mapped toplevel with it.
        std::string app;
        in >> app;
        lx::compositor::toplevel_record const* found = nullptr;
        g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
            if (r.app_id && app == r.app_id && r.mapped && (!found || r.id.id() > found->id.id()))
                found = &r;
        });
        reply = found ? std::format("ok 1 {} {} {} {}", found->geometry.x, found->geometry.y,
                                    found->geometry.width, found->geometry.height)
                      : "ok 0 0 0 0 0";
    } else if (cmd == "usable") {
        const lx::rect2i u = g_comp->windows().usable_area(0);
        reply = std::format("ok {} {} {} {}", u.x, u.y, u.width, u.height);
    } else if (cmd == "move") {
        double x = 0, y = 0;
        in >> x >> y;
        lx::input::pointer_motion_event m{};
        m.x = x;
        m.y = y;
        g_comp->inject_pointer_motion(m);
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
        if (now_ms() - t0 > 120'000.0) {
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
    }
    ::close(sv[0]);
    if (!WIFEXITED(status)) {
        std::printf("  FAIL  %s: child died (status %#x)\n", tier, status);
        return 1;
    }
    return WEXITSTATUS(status);
}

// ════════════════════════════════════════════════════════════════════════════════════
// The child: one lx.ui.host context, surfaces of every role
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
        const bool counting = g_counting;
        g_counting = false; // the test's own request is not the UI's
        (void)::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
        char buf[512];
        const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        g_counting = counting;
        if (n <= 0)
            return "fail";
        buf[n] = '\0';
        return buf;
    }
};

[[nodiscard]] lx::rect2i query_rect(parent_link& p, const std::string& cmd, bool* mapped = nullptr) {
    const std::string r = p.call(cmd);
    lx::rect2i out{};
    if (r.rfind("ok ", 0) != 0)
        return out;
    std::istringstream in{r.substr(3)};
    if (cmd.rfind("record", 0) == 0) {
        int m = 0;
        in >> m;
        if (mapped)
            *mapped = m != 0;
    }
    in >> out.x >> out.y >> out.width >> out.height;
    return out;
}

void click(parent_link& p, double x, double y) {
    (void)p.call(std::format("move {} {}", x, y));
    (void)p.call("button 1");
    (void)p.call("button 0");
}

// ── the loop ──

lx::runtime::event_loop* g_loop = nullptr;

/// Runs the loop until `done()` holds or `timeout_ms` passes (a 1 ms poll: test plumbing).
template <typename F>
bool run_until(F done, int timeout_ms) {
    if (done())
        return true;
    lx::runtime::event_loop& loop = *g_loop;
    bool met = false;
    lx::runtime::timer_source poll{1'000'000, lx::runtime::task{[&] {
                                       if (done()) {
                                           met = true;
                                           loop.quit(0);
                                       }
                                   }}};
    lx::runtime::timer_source limit{0, lx::runtime::task{[&] { loop.quit(0); }}, false};
    limit.schedule_at(loop.now() + static_cast<long long>(timeout_ms) * 1'000'000);
    loop.add_source(&poll);
    loop.add_source(&limit);
    (void)loop.run();
    loop.remove_source(&limit);
    loop.remove_source(&poll);
    return met || done();
}

/// Runs the loop for `ms` with nothing but one quit timer (what an idle client does).
void run_for(int ms) {
    lx::runtime::event_loop& loop = *g_loop;
    lx::runtime::timer_source quit{0, lx::runtime::task{[&loop] { loop.quit(0); }}, false};
    quit.schedule_at(loop.now() + static_cast<long long>(ms) * 1'000'000);
    loop.add_source(&quit);
    (void)loop.run();
    loop.remove_source(&quit);
}

[[nodiscard]] bool settled(host::surface& s) {
    return !s.frame_due() && !s.wl().frame_pending() && !s.root().wants_frame() &&
           !s.input().motion_pending();
}

// ── the UIs ──

lx::ui::child describe_fill(lx::ui::tok bg, const char* label) {
    using namespace lx::ui;
    const child kids[1] = {button(label).name(label)};
    return div(style{}.size_full().bg(bg).flex_col().items_center().justify_center(), kids);
}

lx::ui::child describe_a(lx::ui::build_context&) {
    return describe_fill(lx::ui::tok::element_selection_background, "A");
}
lx::ui::child describe_b(lx::ui::build_context&) {
    return describe_fill(lx::ui::tok::panel_background, "B");
}
lx::ui::child describe_c(lx::ui::build_context&) {
    return describe_fill(lx::ui::tok::surface_background, "C");
}
lx::ui::child describe_panel(lx::ui::build_context&) {
    return describe_fill(lx::ui::tok::title_bar_background, "panel");
}
lx::ui::child describe_menu(lx::ui::build_context&) {
    return describe_fill(lx::ui::tok::elevated_surface_background, "menu");
}
/// A translucent surface: a 40 × 40 opaque square in its middle, nothing elsewhere.
lx::ui::child describe_badge(lx::ui::build_context&) {
    using namespace lx::ui;
    using lx::layout::px;
    const child kids[1] = {div(style{}.flex_none().w(px(40.f)).h(px(40.f)).bg(tok::text_accent), {})};
    return div(style{}.size_full().flex_col().items_center().justify_center(), kids);
}

struct toplevel_surface {
    host::surface surface{};
    host::toplevel_role role{};
};

[[nodiscard]] lx::result<void> open_toplevel(host::context& ctx, toplevel_surface& t,
                                             const char* app_id, lx::size2i size,
                                             lx::ui::describe_fn describe) {
    host::surface_options so{};
    so.describe = describe;
    so.size = size;
    so.frame_records = false;
    if (auto made = t.surface.create(ctx, so); !made)
        return made;
    lx::wayland::toplevel_options to{};
    to.title = app_id;
    to.app_id = app_id;
    to.server_side_decorations = false;
    if (auto made = t.role.create(t.surface, to); !made)
        return made;
    return t.surface.show();
}

[[nodiscard]] unsigned open_fds() {
    unsigned n = 0;
    if (DIR* d = ::opendir("/proc/self/fd")) {
        while (::readdir(d))
            ++n;
        ::closedir(d);
    }
    return n;
}

/// The CPU image's pixel at (x, y) (0: none).
[[nodiscard]] std::uint32_t pixel(const host::surface& s, int x, int y) {
    const lx::gfx::pixel_surface* px = s.cpu_pixels();
    if (!px || x < 0 || y < 0 || static_cast<unsigned>(x) >= px->width ||
        static_cast<unsigned>(y) >= px->height)
        return 0;
    const auto* row = reinterpret_cast<const std::uint32_t*>(px->pixels + static_cast<std::size_t>(y) * px->stride);
    return row[x];
}

/// `xrgb` is token `t` over the window background (sRGB-encoded blend, as the cpu tier's).
[[nodiscard]] bool near_token(std::uint32_t xrgb, lx::ui::tok t) {
    const lx::color c = lx::ui::active_theme().color(t);
    const lx::color bg = lx::ui::active_theme().color(lx::ui::tok::background);
    const auto mix = [&](float v, float under) { return v * c.a + under * (1.f - c.a); };
    const int want[3] = {static_cast<int>(std::lround(mix(c.r, bg.r) * 255.f)),
                         static_cast<int>(std::lround(mix(c.g, bg.g) * 255.f)),
                         static_cast<int>(std::lround(mix(c.b, bg.b) * 255.f))};
    const int got[3] = {static_cast<int>((xrgb >> 16) & 0xFF), static_cast<int>((xrgb >> 8) & 0xFF),
                        static_cast<int>(xrgb & 0xFF)};
    for (unsigned i = 0; i < 3; ++i)
        if (std::abs(want[i] - got[i]) > 2)
            return false;
    return true;
}

/// Another client's window: where a click outside the popup's client lands (a press on the
/// popup client's own surfaces does not end its grab — xdg_popup.grab).
struct other_client {
    lx::wayland::display d{};
    lx::wayland::surface s{};
    lx::wayland::toplevel t{};
    lx::wayland::shm_pool pool{};
    lx::wayland::buffer b{};
    struct log final : lx::wayland::toplevel_handler {
        unsigned configures = 0;
        std::uint32_t serial = 0;
        void on_configure(lx::wayland::toplevel&, const lx::wayland::toplevel_configure& c) override {
            ++configures;
            serial = c.serial;
        }
    } events{};

    bool open() {
        if (!d.connect({.name = k_socket, .track_outputs = false, .default_dmabuf_feedback = false}).ok() ||
            !s.create(d).ok() || !t.create(d, s, {.app_id = k_app_other}).ok())
            return false;
        t.set_handler(&events);
        s.commit();
        for (int i = 0; i < 200 && events.configures == 0; ++i)
            if (!d.roundtrip().ok())
                return false;
        if (events.configures == 0)
            return false;
        t.ack_configure(events.serial);
        if (!pool.create(d, 96 * 96 * 4).ok())
            return false;
        auto* px = reinterpret_cast<std::uint32_t*>(pool.data());
        for (int i = 0; i < 96 * 96; ++i)
            px[i] = 0xFF206040u;
        if (!b.create_shm(pool, 0, {96, 96}, 96 * 4, 0).ok())
            return false;
        s.attach(&b);
        s.damage_buffer({0, 0, 96, 96});
        s.commit();
        return d.roundtrip().ok();
    }
    void close() {
        t.destroy();
        b.destroy();
        pool.destroy();
        s.destroy();
        d.disconnect();
    }
};

void popup_done_cb(void* user) { ++*static_cast<unsigned*>(user); }

int client_main(const char* tier_name, int fd) {
    g_tier = tier_name;
    ::unsetenv("LUMEN_UI_BACKEND");
    parent_link parent{fd};
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    const lx::gfx::client_tier want = lx::gfx::parse_client_tier(tier_name);

    lx::runtime::event_loop loop{};
    g_loop = &loop;
    lx::wayland::display d{};
    if (auto c = d.connect({.name = k_socket}); !c) {
        check("connect", false, lx::format_error(c.get_error()));
        return g_failed;
    }
    bool lost = false;
    lx::wayland::display_source source{d, loop, lx::runtime::task{[&] {
                                           lost = true;
                                           loop.quit(1);
                                       }}};
    source.attach();
    host::context ctx{};
    host::context_options co{};
    co.backend = want;
    co.theme = "one-dark";
    co.watch_theme = 0;
    co.async_icons = false;
    // The headless compositor names no main device, so the tier would take the first
    // render node — the discrete card on a hybrid host, not the one that drives the
    // display. Choose on the test's node instead (the seam exists for this).
    if (const int node = lumen_test::open_render_node(); node >= 0) {
        struct stat st{};
        if (::fstat(node, &st) == 0)
            co.device_override = static_cast<std::uint64_t>(st.st_rdev);
    }
    if (auto s = ctx.start(d, loop, co); !s) {
        check("context starts", false, lx::format_error(s.get_error()));
        return g_failed;
    }

    // ── (a) two toplevels on one context ──
    toplevel_surface a{}, b{};
    {
        auto ma = open_toplevel(ctx, a, k_app_a, {320, 200}, &describe_a);
        auto mb = ma ? open_toplevel(ctx, b, k_app_b, {300, 180}, &describe_b) : ma;
        const bool presented =
            ma && mb &&
            run_until([&] { return a.surface.timeline().first_presented_ns && b.surface.timeline().first_presented_ns; },
                      5000);
        check("two toplevels on one context map and present, on the tier asked for",
              presented && ctx.surface_count() == 2 && a.surface.tier() == want &&
                  a.surface.logical_size().width == 320 && b.surface.logical_size().width == 300,
              ma && mb ? std::format("presented {} surfaces {} tier {} sizes {}×{} {}×{}", presented,
                                     ctx.surface_count(), lx::gfx::client_tier_name(a.surface.tier()),
                                     a.surface.logical_size().width, a.surface.logical_size().height,
                                     b.surface.logical_size().width, b.surface.logical_size().height)
                       : lx::format_error((ma ? mb : ma).get_error()));
        if (!presented)
            return g_failed + 1;
    }
    // (e) one client_gpu, renderer, atlas and glyph cache for both.
    check("a second surface adds no client_gpu, atlas or glyph cache (pointer identity)",
          &a.surface.gpu() == &b.surface.gpu() && &a.surface.atlas() == &b.surface.atlas() &&
              &a.surface.atlas() == &ctx.atlas() &&
              lx::ui::active_ui_resources().text.glyphs == &ctx.glyphs() &&
              lx::ui::active_ui_resources().icons == &ctx.icons() &&
              &a.surface.ui() != &b.surface.ui(),
          std::format("gpu {} atlas {} glyphs {} own ui_context {}", &a.surface.gpu() == &b.surface.gpu(),
                      &a.surface.atlas() == &b.surface.atlas(),
                      lx::ui::active_ui_resources().text.glyphs == &ctx.glyphs(),
                      &a.surface.ui() != &b.surface.ui()));
    {
        const lx::rect2i ra = query_rect(parent, std::string{"record "} + k_app_a);
        const lx::rect2i rb = query_rect(parent, std::string{"record "} + k_app_b);
        // Points only one window covers whatever their order: A's top-left, B's bottom-right
        // (the compositor cascades B over A).
        const lx::point2i pa{ra.x + 8, ra.y + 8};
        const lx::point2i pb{rb.x + rb.width - 8, rb.y + rb.height - 8};
        const bool apart = !(pa.x >= rb.x && pa.x < rb.x + rb.width && pa.y >= rb.y && pa.y < rb.y + rb.height) &&
                           !(pb.x >= ra.x && pb.x < ra.x + ra.width && pb.y >= ra.y && pb.y < ra.y + ra.height);
        // Pointer routed by target: from outside both, into A, then into B.
        (void)parent.call("move 2 790");
        (void)run_until([&] { return !a.surface.input().pointer_inside() && !b.surface.input().pointer_inside(); },
                        2000);
        const host::input_stats a0 = a.surface.input().stats();
        const host::input_stats b0 = b.surface.input().stats();
        (void)parent.call(std::format("move {} {}", pa.x, pa.y));
        (void)run_until([&] { return a.surface.input().pointer_inside(); }, 2000);
        const bool in_a = a.surface.input().pointer_inside() && !b.surface.input().pointer_inside() &&
                          b.surface.input().stats().pointer_frames == b0.pointer_frames;
        (void)parent.call(std::format("move {} {}", pb.x, pb.y));
        (void)run_until([&] { return b.surface.input().pointer_inside(); }, 2000);
        const bool in_b = b.surface.input().pointer_inside() && !a.surface.input().pointer_inside();
        check("pointer: routed by target surface (into A, then into B; A sees its leave)",
              apart && in_a && in_b && a.surface.input().stats().pointer_frames > a0.pointer_frames,
              std::format("A {}×{}+{}+{} B {}×{}+{}+{} in A {} in B {}", ra.width, ra.height, ra.x,
                          ra.y, rb.width, rb.height, rb.x, rb.y, in_a, in_b));

        // Keyboard focus by click, keys to the focused surface only.
        click(parent, pa.x, pa.y);
        (void)run_until([&] { return ctx.keyboard_focus() == &a.surface; }, 2000);
        const std::uint64_t ka = a.surface.input().stats().keys, kb = b.surface.input().stats().keys;
        (void)parent.call("key 30 1"); // KEY_A
        (void)parent.call("key 30 0");
        (void)run_until([&] { return a.surface.input().stats().keys >= ka + 2; }, 2000);
        const bool keys_a = a.surface.input().stats().keys == ka + 2 && b.surface.input().stats().keys == kb;
        click(parent, pb.x, pb.y);
        (void)run_until([&] { return ctx.keyboard_focus() == &b.surface; }, 2000);
        (void)parent.call("key 30 1");
        (void)parent.call("key 30 0");
        (void)run_until([&] { return b.surface.input().stats().keys >= kb + 2; }, 2000);
        const bool keys_b = b.surface.input().stats().keys == kb + 2 && a.surface.input().stats().keys == ka + 2;
        check("keyboard: focus follows the click, keys go to the focused surface only",
              keys_a && keys_b && ctx.keyboard_focus() == &b.surface,
              std::format("keys to A {} then to B {}, focus on {}", keys_a, keys_b,
                          ctx.keyboard_focus() == &b.surface   ? "B"
                          : ctx.keyboard_focus() == &a.surface ? "A"
                                                               : "none"));
        // Modifiers: shift held on B, then the focus moves to A — A has it.
        (void)parent.call("key 42 1"); // KEY_LEFTSHIFT
        (void)run_until([&] { return b.surface.root().pointer().modifiers() != 0; }, 2000);
        const unsigned mod_b = b.surface.root().pointer().modifiers();
        click(parent, pa.x, pa.y);
        (void)run_until([&] { return ctx.keyboard_focus() == &a.surface; }, 2000);
        const unsigned mod_a = a.surface.root().pointer().modifiers();
        (void)parent.call("key 42 0");
        (void)run_until([&] { return a.surface.root().pointer().modifiers() == 0; }, 2000);
        check("modifiers: shift held on B is delivered to A when A takes the focus, and its "
              "release too",
              mod_b != 0 && mod_a == mod_b && ctx.keyboard_focus() == &a.surface &&
                  a.surface.root().pointer().modifiers() == 0,
              std::format("B {:#x} A {:#x} after release {:#x}", mod_b, mod_a,
                          a.surface.root().pointer().modifiers()));

        // Pixels (cpu): each surface its own fill — no cross-talk through the shared renderer.
        if (want == lx::gfx::client_tier::cpu) {
            (void)parent.call("move 2 790");
            (void)run_until([&] { return settled(a.surface) && settled(b.surface); }, 3000);
            const std::uint32_t pa = pixel(a.surface, 4, 4);
            const std::uint32_t pb = pixel(b.surface, 4, 4);
            check("cpu pixels: A and B each show their own fill (shared renderer and atlas, own "
                  "images)",
                  near_token(pa, lx::ui::tok::element_selection_background) &&
                      near_token(pb, lx::ui::tok::panel_background) && pa != pb,
                  std::format("A {:#08x} B {:#08x}", pa, pb));
        }

        // Idle: both settled, nothing requested for 1 s, the timer disarmed.
        (void)parent.call("move 2 790");
        (void)run_until([&] { return settled(a.surface) && settled(b.surface) && ctx.armed_deadline_ns() == 0; },
                        4000);
        run_for(100);
        const std::uint64_t fa = a.surface.stats().frame_requests, fb = b.surface.stats().frame_requests;
        const std::uint64_t ca = a.surface.stats().commits, cb = b.surface.stats().commits;
        const std::uint64_t wake0 = loop.wakeups();
        run_for(1000);
        check("idle: both surfaces request 0 frame callbacks and commit nothing for 1 s; the "
              "timer is disarmed",
              a.surface.stats().frame_requests == fa && b.surface.stats().frame_requests == fb &&
                  a.surface.stats().commits == ca && b.surface.stats().commits == cb &&
                  ctx.armed_deadline_ns() == 0 && loop.wakeups() - wake0 <= 1,
              std::format("requests {} {} commits {} {} wakeups {}",
                          a.surface.stats().frame_requests - fa, b.surface.stats().frame_requests - fb,
                          a.surface.stats().commits - ca, b.surface.stats().commits - cb,
                          loop.wakeups() - wake0));
    }

    // ── (b) a layer surface: the panel ──
    other_client other{};
    const bool have_other = other.open();
    host::surface panel{};
    host::layer_role panel_role{};
    if (!d.has(lx::wayland::global_kind::layer_shell)) {
        skip("layer surface, popup on it", "no zwlr_layer_shell_v1 (or its glue was not built)");
    } else {
        host::surface_options so{};
        so.describe = &describe_panel;
        so.size = {200, 32};
        so.frame_records = false;
        host::layer_role_options lo{};
        lo.layer.level = lx::wayland::layer::top;
        lo.layer.name_space = "lumen-test-panel";
        lo.size = {0, 32};
        lo.anchor = lx::wayland::anchor_top | lx::wayland::anchor_left | lx::wayland::anchor_right;
        lo.exclusive_zone = 32;
        bool made = panel.create(ctx, so).ok() && panel_role.create(panel, lo).ok() && panel.show().ok();
        made = made && run_until([&] { return panel.timeline().first_presented_ns != 0; }, 5000);
        run_for(60);
        const lx::rect2i usable = query_rect(parent, "usable");
        check("layer: a panel configured 1280×32 with zone 32 (usable area starts below it)",
              made && panel.logical_size().width == 1280 && panel.logical_size().height == 32 &&
                  usable.y == 32 && usable.height == 800 - 32,
              std::format("made {} size {}×{} usable {}×{}+{}+{}", made, panel.logical_size().width,
                          panel.logical_size().height, usable.width, usable.height, usable.x, usable.y));

        // Keyboard interactivity switched live: none → on_demand (a click takes the keyboard)
        // → none (a click no longer does).
        click(parent, 600, 16);
        run_for(80);
        const bool none_ignored = ctx.keyboard_focus() != &panel;
        const bool switched = panel_role.set_keyboard_interactivity(lx::wayland::keyboard_interactivity::on_demand);
        run_for(80);
        click(parent, 600, 16);
        const bool took = run_until([&] { return ctx.keyboard_focus() == &panel; }, 2000);
        const std::uint32_t press_serial = ctx.last_press_serial();
        check("layer: keyboard interactivity switched live — on_demand: a click on the panel takes "
              "the keyboard",
              made && none_ignored && switched && took,
              std::format("none ignored {} switched {} took {} (v{})", none_ignored, switched, took,
                          panel_role.layer().version()));

        // ── (c) a popup on the panel with a grab, dismissed by a click outside ──
        host::surface menu{};
        host::popup_role menu_role{};
        unsigned dones = 0;
        lx::wayland::positioner_desc where{};
        where.size = {120, 80};
        where.anchor_rect = {400, 0, 10, 32};
        where.anchor = 6;  // bottom_left
        where.gravity = 8; // bottom_right
        host::surface_options mo{};
        mo.describe = &describe_menu;
        mo.size = {120, 80};
        mo.frame_records = false;
        bool mmade = menu.create(ctx, mo).ok() && menu_role.create(menu, panel_role, where).ok();
        menu_role.set_done_handler(&popup_done_cb, &dones);
        menu_role.grab(press_serial);
        mmade = mmade && menu.show().ok();
        mmade = mmade && run_until([&] { return menu.timeline().first_presented_ns != 0; }, 5000);
        const lx::wayland::popup_configure pc = menu_role.popup().last_configure();
        check("popup on the layer surface: grabbed with the press serial, configured below its "
              "anchor, drawn at 120×80",
              mmade && dones == 0 && pc.geometry.x == 400 && pc.geometry.y == 32 &&
                  menu.logical_size().width == 120 && menu.logical_size().height == 80,
              std::format("made {} geometry {}×{}+{}+{} drawn {}×{} dones {}", mmade, pc.geometry.width,
                          pc.geometry.height, pc.geometry.x, pc.geometry.y, menu.logical_size().width,
                          menu.logical_size().height, dones));
        const lx::rect2i og = query_rect(parent, std::string{"record "} + k_app_other);
        click(parent, og.x + og.width / 2.0, og.y + og.height / 2.0);
        (void)other.d.roundtrip();
        const bool done = run_until([&] { return dones > 0; }, 2000);
        check("popup: a click on another client's window dismisses it (popup_done)",
              have_other && og.width == 96 && done && menu_role.popup().dismissed(),
              std::format("other {}×{}+{}+{} dones {}", og.width, og.height, og.x, og.y, dones));
        menu_role.destroy();
        menu.destroy();
        (void)panel_role.set_keyboard_interactivity(lx::wayland::keyboard_interactivity::none);
    }

    // ── (d) an alpha subsurface placed above A ──
    {
        host::surface badge{};
        host::subsurface_role sub{};
        host::surface_options so{};
        so.describe = &describe_badge;
        so.size = {80, 80};
        so.opaque = false;
        so.frame_records = false;
        so.fractional_scale = false;
        bool made = badge.create(ctx, so).ok() && sub.create(badge, a.surface.wl(), {20, 20}).ok();
        sub.place_above(a.surface.wl());
        made = made && badge.show().ok();
        a.surface.commit_state(); // the position and order are the parent's state
        made = made && run_until([&] { return badge.timeline().first_presented_ns != 0; }, 5000);
        bool alpha_ok = true;
        std::string px_detail = "no cpu pixels on this tier";
        if (want == lx::gfx::client_tier::cpu) {
            const std::uint32_t corner = pixel(badge, 2, 2);
            const std::uint32_t mid = pixel(badge, 40, 40);
            alpha_ok = (corner >> 24) == 0 && (mid >> 24) == 0xFF && near_token(mid, lx::ui::tok::text_accent);
            px_detail = std::format("corner {:#010x} middle {:#010x}", corner, mid);
        }
        check("subsurface: a translucent child placed above its parent draws (transparent where "
              "the UI draws nothing)",
              made && badge.logical_size().width == 80 && badge.swapchain().stats().presents > 0 &&
                  alpha_ok && !d.failed(),
              std::format("made {} size {}×{} {}", made, badge.logical_size().width,
                          badge.logical_size().height, px_detail));
        sub.destroy();
        badge.destroy();
        a.surface.commit_state();
    }

    // ── (f) a surface destroyed mid-frame releases its buffers ──
    {
        const unsigned fds0 = open_fds();
        toplevel_surface c{};
        bool made = open_toplevel(ctx, c, k_app_c, {200, 120}, &describe_c).ok();
        made = made && run_until([&] { return c.surface.timeline().first_presented_ns != 0; }, 5000);
        c.surface.set_continuous(true);
        const std::uint64_t f0 = c.surface.stats().frames;
        (void)run_until([&] { return c.surface.stats().frames >= f0 + 5; }, 3000);
        // Right after a commit: a frame callback outstanding, buffers with the compositor.
        (void)run_until([&] { return c.surface.wl().frame_pending(); }, 1000);
        const bool mid_frame = c.surface.wl().frame_pending();
        const unsigned fds_live = open_fds();
        c.role.destroy();
        c.surface.destroy();
        (void)d.flush();
        run_for(100);
        bool mapped = true;
        (void)query_rect(parent, std::string{"record "} + k_app_c, &mapped);
        const unsigned fds1 = open_fds();
        // The others keep drawing.
        a.surface.root().damage().add(a.surface.root().bounds());
        a.surface.root().damage().request(lx::ui::invalidation::paint);
        const std::uint64_t af = a.surface.stats().frames;
        const bool a_draws = run_until([&] { return a.surface.stats().frames > af; }, 2000);
        check("a surface destroyed mid-frame releases its buffers (its fds closed, unmapped) and "
              "the others keep drawing",
              made && mid_frame && fds1 <= fds0 && !mapped && a_draws && ctx.surface_count() >= 2 && !d.failed(),
              std::format("fds before {} live {} after {}, mapped {}, A drew {}", fds0, fds_live, fds1,
                          mapped, a_draws));
    }

    // ── (g) G5 and (h): both toplevels drawn every frame, 300 frames each ──
    {
        (void)run_until([&] { return settled(a.surface) && settled(b.surface); }, 3000);
        a.surface.set_continuous(true);
        b.surface.set_continuous(true);
        // Warm-up: the frames' first draws (programs, pools) are not steady.
        const std::uint64_t wa = a.surface.stats().frames, wb = b.surface.stats().frames;
        (void)run_until([&] { return a.surface.stats().frames >= wa + 30 && b.surface.stats().frames >= wb + 30; },
                        5000);
        const std::uint64_t fa = a.surface.stats().frames, fb = b.surface.stats().frames;
        const std::uint64_t ea = a.surface.stats().failures, eb = b.surface.stats().failures;
        const std::uint64_t sa = a.surface.stats().slot_wait_ns, sb = b.surface.stats().slot_wait_ns;
        const std::uint64_t new0 = g_new_calls.load(), malloc0 = g_malloc_calls.load();
        g_counting = true;
        const bool ran = run_until(
            [&] { return a.surface.stats().frames >= fa + 300 && b.surface.stats().frames >= fb + 300; }, 30000);
        g_counting = false;
        const std::uint64_t news = g_new_calls.load() - new0;
        const std::uint64_t mallocs = g_malloc_calls.load() - malloc0;
        const std::uint64_t frames = (a.surface.stats().frames - fa) + (b.surface.stats().frames - fb);
        a.surface.set_continuous(false);
        b.surface.set_continuous(false);
#ifndef LUMEN_TEST_SANITIZED
        check("G5: 0 operator new over 300 steady frames per surface (two surfaces drawn together)",
              ran && news == 0,
              std::format("{} frames, {} operator new, {} malloc ({:.1f} per frame: libwayland's "
                          "closures and proxies{})",
                          frames, news, mallocs, frames ? double(mallocs) / double(frames) : 0.0,
                          want == lx::gfx::client_tier::gl ? ", Mesa" : ""));
#else
        skip("G5 allocation count", "sanitizer build");
#endif
        check("two surfaces drawn in the same iterations for 300 frames each: no frame fails",
              ran && a.surface.stats().failures == ea && b.surface.stats().failures == eb && !d.failed() && !lost,
              std::format("A {} frames B {} frames, failures {} {}", a.surface.stats().frames - fa,
                          b.surface.stats().frames - fb, a.surface.stats().failures - ea,
                          b.surface.stats().failures - eb));
        if (want == lx::gfx::client_tier::gl)
            measure(std::format("gl: the shared ring's slot waits over 300 frames each: A {:.2f} ms, "
                                "B {:.2f} ms",
                                (a.surface.stats().slot_wait_ns - sa) / 1e6,
                                (b.surface.stats().slot_wait_ns - sb) / 1e6));
    }

    if (have_other)
        other.close();
    panel_role.destroy();
    panel.destroy();
    a.role.destroy();
    a.surface.destroy();
    b.role.destroy();
    b.surface.destroy();
    ctx.stop();
    source.detach();
    d.disconnect();
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
    cfg.virtual_outputs = "1280x800";
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

    int failed = 0;
    std::printf("tier cpu:\n");
    failed += run_child("cpu");
    if (lumen_test::open_render_node() >= 0) {
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
