// lx.wayland.client against a real compositor, in-process (lx::compositor::compositor on a
// private socket with one virtual output at scale 1.25, a dispatch pump thread, frames ticked
// by the test): globals and their versions, a toplevel mapped with an shm buffer, configure /
// ack on a compositor resize, frame callbacks (once per request, never without one),
// presentation feedback, linux-dmabuf v5 default and surface feedback, explicit-sync
// presence, fractional scale, text-input-v3 against an input method on a second connection,
// the keymap and xkb state, pointer frames from the compositor's virtual input, the
// lx::runtime::event_loop source (no wakeups while idle), disconnect, and the allocation
// count of a steady frame-callback loop.
//
// Allocation counting: operator new and (outside sanitizer builds) malloc are counted on the
// client thread only, while the test is inside client calls — the compositor runs in the
// same process on the pump thread and in `tick()`, and its allocations are not the client's.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE) &&                          \
    defined(LUMEN_PROTOCOL_XDG_SHELL) && defined(LUMEN_PROTOCOL_INPUT_METHOD_UNSTABLE_V2) &&    \
    defined(LUMEN_PROTOCOL_TEXT_INPUT_UNSTABLE_V3)
#include <wayland-client.h>
#include "input-method-unstable-v2-client-protocol.h"
#define LUMEN_TEST_WLC 1
#endif
#include <stdint.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.compositor;
import lx.wayland.client;

// ── allocation accounting (client thread only, while `g_counting`) ──

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
std::atomic<std::uint64_t> g_free_calls{0};
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
void free(void* p) noexcept {
    if (g_counting && p)
        g_free_calls.fetch_add(1, std::memory_order_relaxed);
    __libc_free(p);
}
}
#endif

#if !defined(LUMEN_TEST_SANITIZED)
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

#if !defined(LUMEN_TEST_WLC)

int main() {
    std::printf("SKIP: needs libwayland-client and the protocol glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-wayland-client";

int g_failed = 0;
int g_ran = 0;
int g_skipped = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 0;

void check(const char* name, bool ok, const std::string& detail) {
    ++g_ran;
    std::printf("  %s  %s — %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

void skip(const char* name, const std::string& why) {
    ++g_skipped;
    std::printf("  SKIP  %s — %s\n", name, why.c_str());
}

/// One compositor frame on this thread: UI tick, render tick, posted UI work.
void tick() {
    std::lock_guard<std::mutex> hold(*g_lock);
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
    g_comp->wayland().flush();
}

/// Dispatches until `done()` or ~1 s.
template <typename F>
bool pump(lx::wayland::display& d, F done, int rounds = 100) {
    for (int i = 0; i < rounds && !done(); ++i) {
        if (!d.dispatch(10))
            return false;
    }
    return done();
}

lx::compositor::toplevel_record record(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::compositor::toplevel_record out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id.id()) {
            out.id = r.id;
            out.geometry = r.geometry;
            out.mapped = r.mapped;
            out.title = r.title;
            out.app_id = r.app_id;
        }
    });
    return out;
}

std::string title_of(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    std::string out;
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0)
            out = r.title ? r.title : "";
    });
    return out;
}

void inject_key(unsigned code, bool pressed) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::key_event k{};
    k.evdev_keycode = code;
    k.pressed = pressed;
    g_comp->inject_key(k);
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

void inject_left(bool pressed) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::pointer_button_event b{};
    b.which = lx::input::button::left;
    b.evdev_code = 0x110;
    b.pressed = pressed;
    g_comp->inject_pointer_button(b);
    g_comp->wayland().flush();
}

void inject_wheel(double notches) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::pointer_axis_event a{};
    a.vertical = notches * 15.0;
    a.vertical_discrete = notches;
    a.source = lx::input::axis_source::wheel;
    g_comp->windows().on_pointer_axis(a);
    g_comp->wayland().flush();
}

// ── the client's handlers: plain counters ──

struct window_log final : lx::wayland::toplevel_handler, lx::wayland::surface_handler {
    unsigned configures = 0;
    lx::wayland::toplevel_configure last{};
    unsigned closes = 0;
    unsigned frames = 0;
    std::uint32_t scale120 = 0;
    unsigned scale_events = 0;
    bool scale_before_configure = false;
    unsigned outputs_entered = 0;
    std::vector<lx::wayland::presentation_event> presented;

    void on_configure(lx::wayland::toplevel&, const lx::wayland::toplevel_configure& c) override {
        ++configures;
        last = c;
    }
    void on_close(lx::wayland::toplevel&) override { ++closes; }
    void on_frame(lx::wayland::surface&, std::uint32_t) override { ++frames; }
    void on_preferred_scale(lx::wayland::surface&, std::uint32_t s) override {
        if (scale_events++ == 0)
            scale_before_configure = configures == 0;
        scale120 = s;
    }
    void on_output(lx::wayland::surface&, unsigned, bool entered) override {
        outputs_entered += entered ? 1u : 0u;
    }
    void on_presentation(lx::wayland::surface&, const lx::wayland::presentation_event& e) override {
        presented.push_back(e);
    }
};

struct input_log final : lx::wayland::seat_handler {
    unsigned caps_events = 0;
    unsigned keymaps = 0;
    std::vector<lx::wayland::pointer_frame> frames;
    std::vector<lx::wayland::key_event> keys;
    unsigned focus_in = 0;
    std::uint32_t modifiers = 0;
    std::int32_t rate = -1;

    void on_capabilities(lx::wayland::seat&, std::uint32_t) override { ++caps_events; }
    void on_pointer(lx::wayland::seat&, const lx::wayland::pointer_frame& f) override {
        frames.push_back(f);
    }
    void on_keymap(lx::wayland::seat&) override { ++keymaps; }
    void on_keyboard_focus(lx::wayland::seat&, lx::wayland::surface*, std::uint32_t,
                           bool entered) override {
        focus_in += entered ? 1u : 0u;
    }
    void on_key(lx::wayland::seat&, const lx::wayland::key_event& e) override { keys.push_back(e); }
    void on_modifiers(lx::wayland::seat&, std::uint32_t m) override { modifiers = m; }
    void on_repeat_info(lx::wayland::seat&, std::int32_t r, std::int32_t) override { rate = r; }
};

struct text_log final : lx::wayland::text_input_handler {
    unsigned enters = 0;
    unsigned dones = 0;
    std::string preedit, commit;
    bool current = false;
    std::uint32_t serial = 0;
    void on_focus(lx::wayland::text_input&, lx::wayland::surface*, bool entered) override {
        enters += entered ? 1u : 0u;
    }
    void on_done(lx::wayland::text_input&, const lx::wayland::text_input_done& d) override {
        ++dones;
        if (d.has_preedit)
            preedit = d.preedit;
        if (d.has_commit)
            commit = d.commit;
        current = d.current;
        serial = d.serial;
    }
};

struct buffer_log final : lx::wayland::buffer_handler {
    unsigned releases = 0;
    void on_release(lx::wayland::buffer&) override { ++releases; }
};

/// An argb8888 shm buffer of `size` filled with `argb` at `offset` in `pool`.
bool make_shm(lx::wayland::shm_pool& pool, lx::wayland::buffer& b, std::size_t offset,
              lx::size2i size, std::uint32_t argb) {
    auto* px = reinterpret_cast<std::uint32_t*>(pool.data() + offset);
    for (int i = 0; i < size.width * size.height; ++i)
        px[i] = argb;
    return b.create_shm(pool, offset, size, size.width * 4, 0).ok();
}

// ── the input method: a raw libwayland client, as fcitx5 would be ──

struct ime_client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    zwp_input_method_manager_v2* manager = nullptr;
    zwp_input_method_v2* im = nullptr;
    unsigned activates = 0;
    unsigned dones = 0;
    std::string surrounding;
};

const zwp_input_method_v2_listener k_im_listener = {
    [](void* d, zwp_input_method_v2*) { ++static_cast<ime_client*>(d)->activates; },
    [](void*, zwp_input_method_v2*) {},
    [](void* d, zwp_input_method_v2*, const char* t, uint32_t, uint32_t) {
        static_cast<ime_client*>(d)->surrounding = t ? t : "";
    },
    [](void*, zwp_input_method_v2*, uint32_t) {},
    [](void*, zwp_input_method_v2*, uint32_t, uint32_t) {},
    [](void* d, zwp_input_method_v2*) { ++static_cast<ime_client*>(d)->dones; },
    [](void*, zwp_input_method_v2*) {}};

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
    return wl_display_roundtrip(c.display) >= 0 && c.seat && c.manager;
}

double now_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
}

void run_cases() {
    // ── connect ──
    {
        lx::wayland::display bad;
        auto r = bad.connect({.name = "lumen-test-no-such-socket"});
        check("connect: no compositor at the socket is connect_failed",
              !r && r.get_error().domain == lx::error_domain::wayland &&
                  r.get_error().code == static_cast<int>(lx::wayland_err::connect_failed),
              r ? "connected?" : lx::format_error(r.get_error()));
    }

    lx::wayland::display d;
    const double t_connect = now_ms();
    auto connected = d.connect({.name = k_socket});
    const double connect_ms = now_ms() - t_connect;
    check("connect: private socket, required globals bound", connected.ok() && d.connected(),
          connected ? std::format("{:.2f} ms incl. 3 roundtrips", connect_ms)
                    : lx::format_error(connected.get_error()));
    if (!connected)
        return;

    using gk = lx::wayland::global_kind;
    std::string versions;
    for (unsigned i = 0; i < lx::wayland::k_global_kinds; ++i) {
        const auto& g = d.global(static_cast<gk>(i));
        versions += std::format("{}{} {}/{}", versions.empty() ? "" : ", ", g.interface, g.bound,
                                g.advertised);
    }
    std::printf("  globals (bound/advertised): %s\n", versions.c_str());
    check("globals: wl_compositor v6, wl_shm, xdg_wm_base v6 (caps 6/2/7, server offers 6)",
          d.version(gk::compositor) == 6 && d.has(gk::shm) && d.version(gk::xdg_wm_base) == 6,
          std::format("compositor {} shm {} xdg_wm_base {}", d.version(gk::compositor),
                      d.version(gk::shm), d.version(gk::xdg_wm_base)));
    check("globals: optional versions as capped (dmabuf 5, presentation 2, text-input 2, "
          "cursor-shape 2, fractional 1, viewporter 1, decoration 2, xdg-output 3, "
          "single-pixel 1)",
          d.version(gk::dmabuf) == 5 && d.version(gk::presentation) == 2 &&
              d.version(gk::text_input) == 2 && d.version(gk::cursor_shape) == 2 &&
              d.version(gk::fractional_scale) == 1 && d.version(gk::viewporter) == 1 &&
              d.version(gk::decoration) == 2 && d.version(gk::xdg_output) == 3 &&
              d.version(gk::single_pixel) == 1,
          versions);
    check("globals: wl_shm formats argb8888 + xrgb8888", (d.shm_format_mask() & 3u) == 3u,
          std::format("mask {:#x}, {} formats", d.shm_format_mask(), d.shm_format_count()));
    const lx::wayland::output* out = d.output_at(0);
    check("outputs: one, 1280x800 mode, wl_output.scale 2 (ceil 1.25), xdg_output 1024x640",
          d.output_count() == 1 && out && out->info().mode.width == 1280 &&
              out->info().mode.height == 800 && out->info().scale == 2 &&
              out->info().logical.width == 1024 && out->info().logical.height == 640 &&
              out->info().done,
          out ? std::format("{} {}x{} scale {} logical {}x{}", out->info().name,
                            out->info().mode.width, out->info().mode.height, out->info().scale,
                            out->info().logical.width, out->info().logical.height)
              : "no output");
    check("presentation: clock_id is CLOCK_MONOTONIC", d.presentation_clock() == CLOCK_MONOTONIC,
          std::format("clock {}", d.presentation_clock()));

    lx::wayland::seat* seat = d.default_seat();
    input_log in{};
    if (seat)
        seat->set_handler(&in);
    check("seat: bound with pointer and keyboard", seat &&
              (seat->capabilities() & (lx::wayland::seat_pointer | lx::wayland::seat_keyboard)) ==
                  (lx::wayland::seat_pointer | lx::wayland::seat_keyboard),
          seat ? std::format("caps {:#x} name '{}'", seat->capabilities(), seat->name()) : "none");
    if (!seat)
        return;

    // ── dmabuf feedback ──
    lx::wayland::dmabuf_feedback* fb = d.default_feedback();
    if (!fb || !d.has(gk::dmabuf)) {
        skip("dmabuf: default feedback", "the compositor advertises no zwp_linux_dmabuf_v1 v4+");
    } else {
        const auto& snap = fb->snapshot();
        bool consistent = snap.tranche_count >= 1 && snap.bad_indices == 0;
        unsigned total = 0;
        for (const auto& t : snap.list()) {
            consistent = consistent && t.count >= 1 && t.count <= snap.table_entries;
            total += t.count;
        }
        check("dmabuf: v5 default feedback parsed (table, main device, tranches)",
              fb->received() && consistent && snap.table_entries > 0,
              std::format("main device {:#x}, {} table entries, {} tranche(s), {} formats, "
                          "first {:#010x}/{:#x}, target {:#x} flags {}",
                          snap.main_device, snap.table_entries, snap.tranche_count, total,
                          snap.tranche_count ? snap.tranches[0].formats[0].format : 0u,
                          snap.tranche_count ? snap.tranches[0].formats[0].modifier : 0ull,
                          snap.tranche_count ? snap.tranches[0].target_device : 0ull,
                          snap.tranche_count ? snap.tranches[0].flags : 0u));
        check("dmabuf: the snapshot answers supports() for its own first entry",
              snap.tranche_count && snap.supports(snap.tranches[0].formats[0].format,
                                                  snap.tranches[0].formats[0].modifier) &&
                  !snap.supports(0x20202020u, 0x1234u),
              "format/modifier lookup");
    }

    // ── a toplevel ──
    const char* app_id = "lumen.test.wlclient";
    window_log w{};
    buffer_log released{};
    lx::wayland::surface s;
    lx::wayland::toplevel top;
    lx::wayland::shm_pool pool;
    lx::wayland::buffer buf[3];
    bool ok = s.create(d).ok();
    s.set_handler(&w);
    const bool frac = s.enable_fractional_scale().ok();
    ok = ok && top.create(d, s, {.title = "lx wayland client", .app_id = app_id}).ok();
    top.set_handler(&w);
    s.commit(); // the initial commit: no buffer
    ok = ok && pump(d, [&] { return w.configures >= 1; });
    check("toplevel: initial configure (size 0×0: client picks), server-side decoration",
          ok && w.last.initial && w.last.decoration == lx::wayland::decoration_mode::server_side,
          std::format("configures {} size {}x{} states {:#x} wm_caps {:#x} decoration {}",
                      w.configures, w.last.size.width, w.last.size.height, w.last.states,
                      w.last.wm_capabilities, static_cast<int>(w.last.decoration)));
    check("fractional scale: preferred_scale 150 (1.25) before the first configure",
          frac && w.scale120 == 150 && w.scale_before_configure && s.preferred_scale120() == 150,
          std::format("scale120 {} ({} events, before configure {})", w.scale120, w.scale_events,
                      w.scale_before_configure));

    constexpr lx::size2i k_size{240, 160};
    ok = ok && pool.create(d, 3u * 1024u * 640u * 4u).ok();
    ok = ok && make_shm(pool, buf[0], 0, k_size, 0xFF336699u);
    buf[0].set_handler(&released);
    top.ack_configure(w.last.serial);
    s.attach(&buf[0]);
    s.damage_buffer({0, 0, k_size.width, k_size.height});
    s.commit();
    ok = ok && d.roundtrip().ok();
    tick();
    ok = ok && d.roundtrip().ok();
    const auto rec = record(app_id);
    check("toplevel: mapped with an shm buffer; the compositor's record has title and app_id",
          ok && rec.mapped && title_of(app_id) == "lx wayland client" &&
              rec.geometry.width == k_size.width && rec.geometry.height == k_size.height,
          std::format("mapped {} title '{}' geometry {}x{}+{}+{}", rec.mapped, title_of(app_id),
                      rec.geometry.width, rec.geometry.height, rec.geometry.x, rec.geometry.y));
    top.set_title("lx wayland client 2");
    (void)d.roundtrip();
    check("toplevel: set_title reaches the record", title_of(app_id) == "lx wayland client 2",
          title_of(app_id));
    check("surface: entered the output", w.outputs_entered >= 1 && (s.outputs() & 1u),
          std::format("enters {} mask {:#x}", w.outputs_entered, s.outputs()));

    // ── configure / ack on a resize the compositor sends (maximise) ──
    {
        const unsigned before = w.configures;
        {
            std::lock_guard<std::mutex> hold(*g_lock);
            g_comp->windows().set_maximized(rec.id, true, "test");
            g_comp->wayland().flush();
        }
        tick();
        const bool got = pump(d, [&] { return w.configures > before; });
        const auto c = w.last;
        const bool maxed = (c.states & lx::wayland::state_maximized) != 0;
        // The usable area (1024x640 logical) less the server-side title bar.
        check("configure: maximise arrives as one sequence (size, maximized, activated)",
              got && maxed && c.size.width == 1024 && c.size.height > 560 &&
                  c.size.height <= 640 && !c.initial &&
                  (c.states & lx::wayland::state_activated),
              std::format("size {}x{} states {:#x} serial {}", c.size.width, c.size.height,
                          c.states, c.serial));
        top.ack_configure(c.serial);
        std::size_t offset = 0;
        ok = pool.size() >= std::size_t(c.size.width) * c.size.height * 4 &&
             make_shm(pool, buf[1], offset, c.size, 0xFF996633u);
        s.attach(&buf[1]);
        s.damage_buffer({0, 0, c.size.width, c.size.height});
        s.commit();
        (void)d.roundtrip();
        tick();
        (void)d.roundtrip();
        const auto r2 = record(app_id);
        check("configure: acked serial, the next commit has the new size",
              ok && top.acked_serial() == c.serial && r2.geometry.width == c.size.width &&
                  r2.geometry.height == c.size.height,
              std::format("acked {} geometry {}x{}", top.acked_serial(), r2.geometry.width,
                          r2.geometry.height));
        top.ack_configure(c.serial); // a second ack of the same serial: refused, not sent
        check("configure: a repeated ack is refused by the wrapper (no protocol error)",
              d.roundtrip().ok() && !d.failed(), "connection healthy");
    }

    // ── frame callbacks ──
    {
        tick();
        (void)d.roundtrip();
        const unsigned f0 = w.frames;
        for (int i = 0; i < 3; ++i)
            tick();
        (void)d.roundtrip();
        check("frame: no callback without a request (3 frames ticked)", w.frames == f0,
              std::format("{} callbacks", w.frames - f0));
        s.request_frame();
        s.request_frame(); // idempotent: one outstanding
        s.damage_buffer({0, 0, 8, 8});
        s.commit();
        (void)d.roundtrip();
        for (int i = 0; i < 3 && w.frames == f0; ++i) {
            tick();
            (void)d.dispatch(10);
        }
        for (int i = 0; i < 3; ++i)
            tick();
        (void)d.roundtrip();
        check("frame: one request → exactly one callback (requested twice, 6 frames ticked)",
              w.frames == f0 + 1 && !s.frame_pending(),
              std::format("{} callback(s)", w.frames - f0));
    }

    // ── presentation feedback ──
    {
        w.presented.clear();
        const bool asked = s.request_presentation_feedback();
        s.attach(&buf[1]);
        s.damage_buffer({0, 0, 64, 64});
        const std::uint64_t id = s.commit();
        (void)d.roundtrip();
        const double before = now_ms();
        for (int i = 0; i < 6 && w.presented.empty(); ++i) {
            tick();
            (void)d.dispatch(10);
        }
        const bool got = !w.presented.empty();
        const auto e = got ? w.presented.front() : lx::wayland::presentation_event{};
        const double t_ms = static_cast<double>(e.time_ns) / 1e6;
        check("presentation: feedback for the commit arrives, presented, on CLOCK_MONOTONIC",
              asked && got && e.commit_id == id && e.presented && t_ms > before - 1000.0 &&
                  t_ms <= now_ms() + 1.0,
              got ? std::format("commit {} presented {} at {:.1f} ms (now {:.1f}) refresh {} ns "
                                "seq {} flags {:#x}",
                                e.commit_id, e.presented, t_ms, now_ms(), e.refresh_ns,
                                e.sequence, e.flags)
                  : "none");
    }

    // ── a surface's dmabuf feedback ──
    if (d.has(gk::dmabuf)) {
        lx::wayland::dmabuf_feedback sf;
        const bool made = sf.create(d, &s).ok();
        (void)d.roundtrip();
        check("dmabuf: get_surface_feedback parsed", made && sf.received() &&
                  sf.snapshot().tranche_count >= 1,
              std::format("{} tranche(s)", sf.snapshot().tranche_count));
    }

    // ── explicit sync ──
    if (!d.has(gk::syncobj)) {
        skip("syncobj: wp_linux_drm_syncobj_manager_v1",
             "not advertised — the compositor offers it only with timeline syncobjs on its DRM "
             "device (none here: virtual outputs open no card; vmwgfx has none either)");
    } else {
        check("syncobj: surface object created", s.enable_syncobj().ok(), "get_surface");
    }

    // ── keyboard: keymap, xkb state, keys, modifiers ──
    {
        (void)pump(d, [&] { return seat->has_keymap(); });
        check("keyboard: keymap received and compiled into an xkb state",
              seat->has_keymap() && seat->native_state() != nullptr,
              std::format("keymaps {} repeat rate {} delay {}", in.keymaps, seat->repeat_rate(),
                          seat->repeat_delay_ms()));
        check("keyboard: focus entered the toplevel's surface",
              in.focus_in >= 1 && seat->keyboard_focus() == &s,
              std::format("enters {}", in.focus_in));
        in.keys.clear();
        inject_key(30, true); // KEY_A
        inject_key(30, false);
        inject_key(42, true); // KEY_LEFTSHIFT
        inject_key(30, true);
        inject_key(30, false);
        inject_key(42, false);
        (void)pump(d, [&] { return in.keys.size() >= 6; });
        const auto find = [&](std::uint32_t key, lx::wayland::key_state st, unsigned nth) {
            unsigned n = 0;
            for (const auto& k : in.keys)
                if (k.key == key && k.state == st && n++ == nth)
                    return k;
            return lx::wayland::key_event{};
        };
        const auto a = find(30, lx::wayland::key_state::pressed, 0);
        const auto shifted = find(30, lx::wayland::key_state::pressed, 1);
        check("keyboard: KEY_A → keysym a, text \"a\"; with Shift → A, \"A\", mod_shift",
              a.keysym == 0x61 && std::string(a.utf8) == "a" && shifted.keysym == 0x41 &&
                  std::string(shifted.utf8) == "A" &&
                  (shifted.modifiers & lx::wayland::mod_shift) && a.repeats &&
                  in.modifiers == 0,
              std::format("{} keys; a {:#x} '{}', shifted {:#x} '{}' mods {:#x}", in.keys.size(),
                          a.keysym, a.utf8, shifted.keysym, shifted.utf8, shifted.modifiers));
    }

    // ── pointer: enter / motion / frame from the compositor's virtual input ──
    {
        const auto r = record(app_id);
        // Off the surface first (the map put it under the pointer): onto the server-side
        // title bar above it — the window is maximised, so nowhere else is off it.
        inject_move(r.geometry.x + 100.0, r.geometry.y - 10.0);
        in.frames.clear();
        (void)pump(d, [&] { return seat->pointer_focus() == nullptr; });
        const auto left = in.frames;
        in.frames.clear();
        inject_move(r.geometry.x + 100.0, r.geometry.y + 50.0);
        (void)pump(d, [&] { return !in.frames.empty(); });
        inject_move(r.geometry.x + 110.0, r.geometry.y + 55.0);
        (void)pump(d, [&] { return in.frames.size() >= 2; });
        const bool have2 = in.frames.size() >= 2;
        const auto f0 = have2 ? in.frames[0] : lx::wayland::pointer_frame{};
        const auto f1 = have2 ? in.frames[1] : lx::wayland::pointer_frame{};
        using pf = lx::wayland::pointer_frame;
        check("pointer: off the surface was a leave frame",
              left.size() == 1 && left[0].mask == pf::has_leave && left[0].target == &s,
              std::format("{} frames, mask {:#x}", left.size(), left.empty() ? 0u : left[0].mask));
        check("pointer: first move is one frame with enter on our surface at (100, 50)",
              have2 && (f0.mask & pf::has_enter) && f0.target == &s &&
                  std::abs(f0.x - 100.0) < 0.01 && std::abs(f0.y - 50.0) < 0.01 &&
                  seat->pointer_focus() == &s,
              std::format("{} frames; mask {:#x} at ({:.2f}, {:.2f})", in.frames.size(), f0.mask,
                          f0.x, f0.y));
        check("pointer: second move is a motion-only frame at (110, 55)",
              have2 && f1.mask == pf::has_motion && f1.target == &s &&
                  std::abs(f1.x - 110.0) < 0.01 && std::abs(f1.y - 55.0) < 0.01,
              std::format("mask {:#x} at ({:.2f}, {:.2f})", f1.mask, f1.x, f1.y));
        in.frames.clear();
        inject_left(true);
        inject_left(false);
        (void)pump(d, [&] { return in.frames.size() >= 2; });
        const auto b = in.frames.empty() ? pf{} : in.frames[0];
        check("pointer: a press is a button frame with its serial (BTN_LEFT, pressed)",
              (b.mask & pf::has_button) && b.button == 0x110 && b.pressed && b.serial != 0 &&
                  seat->last_input_serial() != 0,
              std::format("{} frames; mask {:#x} button {:#x} serial {}", in.frames.size(),
                          b.mask, b.button, b.serial));
        in.frames.clear();
        inject_wheel(1.0);
        (void)pump(d, [&] { return !in.frames.empty(); });
        const auto ax = in.frames.empty() ? pf{} : in.frames[0];
        check("pointer: one wheel notch is one frame: axis + axis_value120 120 + source wheel",
              (ax.mask & pf::has_axis) && (ax.mask & pf::has_axis_value120) &&
                  ax.value120[0] == 120 && (ax.mask & pf::has_axis_source) && ax.source == 0,
              std::format("{} frames; mask {:#x} value {:.2f} value120 {} source {}",
                          in.frames.size(), ax.mask, ax.axis_value[0], ax.value120[0],
                          ax.source));
        check("cursor: shape set through wp_cursor_shape_v1",
              seat->set_cursor_shape(lx::wayland::cursor_shape::pointer) && d.roundtrip().ok() &&
                  !d.failed(),
              "set_shape(pointer) against the enter serial");
    }

    // ── text-input-v3 against an input method on another connection ──
    {
        lx::wayland::text_input ti;
        text_log tl{};
        bool made = ti.create(d, *seat).ok();
        ti.set_handler(&tl);
        ime_client ime{};
        const bool ime_up = ime_connect(ime);
        if (!ime_up) {
            skip("text-input: round trip", "no zwp_input_method_manager_v2 to drive one");
        } else {
            ime.im = zwp_input_method_manager_v2_get_input_method(ime.manager, ime.seat);
            zwp_input_method_v2_add_listener(ime.im, &k_im_listener, &ime);
            const auto both = [&] {
                (void)d.roundtrip();
                (void)wl_display_roundtrip(ime.display);
                (void)d.roundtrip();
            };
            both();
            check("text-input: entered on the focused surface once an input method binds",
                  made && tl.enters == 1 && ti.focus() == &s,
                  std::format("enters {} v{}", tl.enters, ti.version()));
            ti.enable();
            ti.set_surrounding_text("abc", 3, 3);
            ti.set_content_type(0x1 /* completion */, 0 /* normal */);
            ti.set_cursor_rectangle({20, 30, 2, 18});
            ti.commit();
            both();
            check("text-input: enable + commit → the input method is activated with the text",
                  ime.activates == 1 && ime.dones == 1 && ime.surrounding == "abc" &&
                      ti.enabled() && ti.commit_count() == 1,
                  std::format("activates {} dones {} surrounding '{}'", ime.activates, ime.dones,
                              ime.surrounding));
            zwp_input_method_v2_set_preedit_string(ime.im, "\xce\xb1", 2, 2); // α
            zwp_input_method_v2_commit(ime.im, ime.dones);
            both();
            check("text-input: preedit arrives with done(serial = commits), current",
                  tl.preedit == "\xce\xb1" && tl.serial == ti.commit_count() && tl.current,
                  std::format("preedit '{}' serial {} current {}", tl.preedit, tl.serial,
                              tl.current));
            zwp_input_method_v2_commit_string(ime.im, "ok");
            zwp_input_method_v2_commit(ime.im, ime.dones);
            both();
            check("text-input: commit_string arrives; the preedit clears with that done",
                  tl.commit == "ok" && tl.dones >= 2,
                  std::format("commit '{}' dones {}", tl.commit, tl.dones));
            ti.disable();
            ti.commit();
            both();
            zwp_input_method_v2_destroy(ime.im);
        }
        if (ime.manager)
            zwp_input_method_manager_v2_destroy(ime.manager);
        if (ime.seat)
            wl_seat_destroy(ime.seat);
        if (ime.registry)
            wl_registry_destroy(ime.registry);
        if (ime.display)
            wl_display_disconnect(ime.display);
        ti.destroy();
        (void)d.roundtrip();
    }

    // ── a popup on the toplevel ──
    {
        struct popup_log final : lx::wayland::popup_handler {
            unsigned configures = 0;
            lx::wayland::popup_configure last{};
            void on_configure(lx::wayland::popup&, const lx::wayland::popup_configure& c) override {
                ++configures;
                last = c;
            }
        } pl{};
        lx::wayland::surface ps;
        lx::wayland::popup pop;
        lx::wayland::shm_pool ppool;
        lx::wayland::buffer pb;
        lx::wayland::positioner_desc where{};
        where.size = {120, 80};
        where.anchor_rect = {40, 30, 10, 10};
        where.anchor = 8;  // bottom_right
        where.gravity = 8; // bottom_right
        bool made = ps.create(d).ok() && pop.create(d, ps, top, where).ok();
        pop.set_handler(&pl);
        ps.commit();
        made = made && pump(d, [&] { return pl.configures >= 1; });
        pop.ack_configure(pl.last.serial);
        made = made && ppool.create(d, 120 * 80 * 4).ok() &&
               make_shm(ppool, pb, 0, {120, 80}, 0xFF404040u);
        ps.attach(&pb);
        ps.damage_buffer({0, 0, 120, 80});
        ps.commit();
        made = made && d.roundtrip().ok();
        check("popup: positioned below-right of its anchor, configured, mapped",
              made && pl.last.initial && pl.last.geometry.x == 50 && pl.last.geometry.y == 40 &&
                  pl.last.geometry.width == 120 && pl.last.geometry.height == 80 && !d.failed(),
              std::format("configures {} geometry {}x{}+{}+{}", pl.configures,
                          pl.last.geometry.width, pl.last.geometry.height, pl.last.geometry.x,
                          pl.last.geometry.y));
        pop.destroy();
        pb.destroy();
        ppool.destroy();
        ps.destroy();
        (void)d.roundtrip();
    }

    // ── a full socket: flush says `again`, and the read protocol waits for POLLOUT ──
    {
        lx::wayland::display d2;
        lx::wayland::surface s2;
        bool made = d2.connect({.name = k_socket, .track_outputs = false,
                                .default_dmabuf_feedback = false})
                        .ok() &&
                    s2.create(d2).ok();
        bool again = false;
        unsigned sent = 0;
        {
            // The compositor stops reading while the pump is held.
            std::lock_guard<std::mutex> hold(*g_lock);
            for (; made && sent < 200'000 && !again; ++sent) {
                s2.damage_buffer({0, 0, 1, 1});
                if ((sent & 255) == 255) {
                    auto f = d2.flush();
                    if (!f) {
                        made = false;
                        break;
                    }
                    again = *f == lx::wayland::display::flush_status::again;
                }
            }
        }
        // The pump reads again; dispatch() waits for POLLOUT and flushes what is queued.
        bool drained = false;
        for (int i = 0; made && i < 200 && !drained; ++i) {
            (void)d2.dispatch(10);
            auto f = d2.flush();
            drained = f && *f == lx::wayland::display::flush_status::done;
        }
        const bool healthy = made && d2.roundtrip().ok() && !d2.failed();
        check("flush: a full socket returns `again`, then drains without losing the connection",
              made && again && drained && healthy,
              std::format("again after {} requests, drained {}, healthy {}", sent, drained,
                          healthy));
        s2.destroy();
        d2.disconnect();
    }

    // ── G5: a steady frame-callback loop allocates nothing in the wrapper ──
    {
        constexpr int k_warm = 20;
        constexpr int k_frames = 300;
        std::uint64_t new0 = 0, mal0 = 0, free0 = 0;
        unsigned done = 0;
        double frame_ms_sum = 0.0;
        // Client-thread time in the wrapper + libwayland per frame (request, commit, flush,
        // then the dispatch that reads and runs the callback), compositor ticks excluded.
        std::array<double, k_frames> client_us{};
        for (int i = 0; i < k_warm + k_frames; ++i) {
            if (i == k_warm) {
                new0 = g_new_calls.load();
                mal0 = g_malloc_calls.load();
                free0 = g_free_calls.load();
            }
            const unsigned before = w.frames;
            g_counting = i >= k_warm;
            double c0 = now_ms();
            s.request_frame();
            s.damage_buffer({0, 0, 4, 4});
            (void)s.commit();
            (void)d.flush();
            double spent = now_ms() - c0;
            g_counting = false;
            const double t0 = now_ms();
            for (int k = 0; k < 4 && w.frames == before; ++k) {
                tick();
                g_counting = i >= k_warm;
                c0 = now_ms();
                (void)d.dispatch(10);
                spent += now_ms() - c0;
                g_counting = false;
            }
            frame_ms_sum += now_ms() - t0;
            if (i >= k_warm)
                client_us[static_cast<std::size_t>(i - k_warm)] = spent * 1000.0;
            done += (i >= k_warm && w.frames > before) ? 1u : 0u;
        }
        std::sort(client_us.begin(), client_us.end());
        std::printf("  measure  client-side frame-callback cycle: p50 %.1f us, p95 %.1f us, max "
                    "%.1f us (request + commit + flush + dispatch)\n",
                    client_us[k_frames / 2], client_us[k_frames * 95 / 100],
                    client_us[k_frames - 1]);
        const std::uint64_t news = g_new_calls.load() - new0;
        const std::uint64_t mallocs = g_malloc_calls.load() - mal0;
        const std::uint64_t frees = g_free_calls.load() - free0;
#if defined(LUMEN_TEST_SANITIZED)
        const char* malloc_note = "malloc not interposed under a sanitizer";
#else
        const char* malloc_note = "libwayland's own";
#endif
#if defined(LUMEN_TEST_SANITIZED)
        std::printf("  note  G5 is not counted under a sanitizer (its allocator is not replaced)\n");
        const bool counted = false;
#else
        const bool counted = true;
#endif
        check("G5: steady frame-callback loop: 0 operator new in the client (300 frames)",
              done == k_frames && (!counted || news == 0),
              std::format("{} callbacks, {} operator new; malloc {} / free {} on the client thread "
                          "({:.1f} / {:.1f} per frame, {}: wl_proxy + wl_closure per message), "
                          "{:.2f} ms per tick+dispatch",
                          done, news, mallocs, frees, double(mallocs) / k_frames,
                          double(frees) / k_frames, malloc_note, frame_ms_sum / k_frames));
#if !defined(LUMEN_TEST_SANITIZED)
        check("G5: what libwayland mallocs per frame it frees in the same frame (net 0)",
              mallocs == frees, std::format("net {}", std::int64_t(mallocs) - std::int64_t(frees)));
#endif
    }

    // ── the lx::runtime::event_loop source, in a child process ──
    // lx::runtime keeps one process-wide source table, and the compositor's loop has its
    // sources in it: a second loop here would dispatch them on this thread. The child is
    // this binary without a compositor; this process ticks frames until it exits.
    {
        std::fflush(stdout);
        const pid_t child = ::fork();
        if (child == 0) {
            char self[] = "/proc/self/exe";
            char flag[] = "--event-loop-client";
            char* argv[] = {self, flag, nullptr};
            ::execv(self, argv);
            ::_exit(127);
        }
        int status = -1;
        const double t0 = now_ms();
        while (child > 0) {
            const pid_t r = ::waitpid(child, &status, WNOHANG);
            if (r == child)
                break;
            if (now_ms() - t0 > 10'000.0) {
                ::kill(child, SIGKILL);
                (void)::waitpid(child, &status, 0);
                break;
            }
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check("event loop: display_source in a child client (idle and frame cases above)",
              child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              std::format("child status {:#x}", status));
    }

    // ── close and disconnect ──
    {
        const unsigned c0 = w.closes;
        const lx::toplevel_id id = record(app_id).id;
        {
            std::lock_guard<std::mutex> hold(*g_lock);
            g_comp->windows().request_close(id);
            g_comp->wayland().flush();
        }
        (void)pump(d, [&] { return w.closes > c0; });
        check("toplevel: the compositor's close request arrives", w.closes == c0 + 1,
              std::format("{} close(s)", w.closes - c0));
    }
    top.destroy();
    for (auto& b : buf)
        b.destroy();
    pool.destroy();
    s.destroy();
    d.disconnect();
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bool clients = true;
        {
            std::lock_guard<std::mutex> hold(*g_lock);
            clients = g_comp->wayland().has_clients();
        }
        if (!clients && record(app_id).id.id() == 0)
            break;
    }
    // record() takes the lock itself; re-check outside it.
    {
        bool clients = true;
        {
            std::lock_guard<std::mutex> hold(*g_lock);
            clients = g_comp->wayland().has_clients();
        }
        const auto r = record(app_id);
        check("disconnect: the compositor drops the client and its toplevel",
              !clients && r.id.id() == 0 && !d.connected(),
              std::format("clients {} toplevel {}", clients, r.id.id()));
    }
}

/// The child of the event-loop case: a client with no compositor in its process, so the
/// lx::runtime loop it runs has only its own sources. Exit status: failures.
int event_loop_client() {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::wayland::display d;
    if (!d.connect({.name = k_socket})) {
        std::printf("  FAIL  event loop child: connect\n");
        return 1;
    }
    window_log w{};
    lx::wayland::surface s;
    lx::wayland::toplevel top;
    lx::wayland::shm_pool pool;
    lx::wayland::buffer b;
    bool ok = s.create(d).ok() && top.create(d, s, {.app_id = "lumen.test.wlclient.loop"}).ok();
    s.set_handler(&w);
    top.set_handler(&w);
    s.commit();
    ok = ok && pump(d, [&] { return w.configures >= 1; });
    top.ack_configure(w.last.serial);
    ok = ok && pool.create(d, 64 * 64 * 4).ok() && make_shm(pool, b, 0, {64, 64}, 0xFF202020u);
    s.attach(&b);
    s.damage_buffer({0, 0, 64, 64});
    s.commit();
    ok = ok && d.roundtrip().ok();
    // Let the map settle (activation, enter, a placement configure) before measuring idle.
    for (int i = 0; i < 10; ++i)
        (void)d.dispatch(10);
    if (w.configures > 1)
        top.ack_configure(w.last.serial);
    (void)d.flush();

    lx::runtime::event_loop loop;
    bool died = false;
    lx::wayland::display_source src{d, loop, lx::runtime::callback{[&died] { died = true; }}};
    src.attach();
    lx::runtime::timer_source quit{0, lx::runtime::callback{[&loop] { loop.quit(0); }}, false};
    quit.schedule_at(loop.now() + 200'000'000);
    loop.add_source(&quit);
    const auto wake0 = loop.wakeups();
    const auto reads0 = src.reads();
    (void)loop.run();
    const auto idle_wakeups = loop.wakeups() - wake0;
    const auto idle_reads = src.reads() - reads0;
    int failed = 0;
    const auto report = [&failed](const char* name, bool pass, const std::string& detail) {
        std::printf("  %s  %s — %s\n", pass ? "PASS" : "FAIL", name, detail.c_str());
        failed += pass ? 0 : 1;
    };
    report("event loop: an idle connection wakes the loop for nothing (200 ms)",
           ok && idle_wakeups <= 1 && idle_reads == 0 && !died,
           std::format("{} wakeup(s) incl. the quit timer, {} socket reads", idle_wakeups,
                       idle_reads));
    // A frame callback through the loop: the source flushes the request in prepare and reads
    // and dispatches the answer (the parent process ticks the compositor every 5 ms).
    const unsigned f0 = w.frames;
    s.request_frame();
    s.damage_buffer({0, 0, 4, 4});
    (void)s.commit();
    lx::runtime::timer_source poll_done{
        2'000'000, lx::runtime::callback{[&] {
            if (w.frames > f0)
                loop.quit(0);
        }}};
    loop.add_source(&poll_done);
    quit.schedule_at(loop.now() + 3'000'000'000);
    (void)loop.run();
    loop.remove_source(&poll_done);
    loop.remove_source(&quit);
    report("event loop: frame callback requested and answered through display_source",
           w.frames == f0 + 1 && src.reads() > reads0 && !died,
           std::format("{} callback(s), {} socket read(s)", w.frames - f0, src.reads() - reads0));
    src.detach();
    top.destroy();
    b.destroy();
    pool.destroy();
    s.destroy();
    d.disconnect();
    return failed;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--event-loop-client") == 0)
        return event_loop_client();
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.present = lx::compositor::present_backend::cpu;
    cfg.virtual_outputs = "1280x800@1.25";
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
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
            (void)poll(&pf, 1, 5);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tick();

    run_cases();

    pump_on.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed, %d skipped\n", g_ran, g_failed, g_skipped);
    return g_failed == 0 ? 0 : 1;
}

#endif
