// lumen-ui-bench --client — ADR-010 §8.3's client-mode scenarios (Phase 4): S1, S5, S7, S9 and
// S10 as real lx.app windows against a running compositor.
//
//   lumen-ui-bench --client [--scenario S1,S5,S7,S9,S10|all] [--tier cpu,gl,vulkan|all]
//                  [--json PATH] [--compositor host|binary] [--connect] [--frames N]
//                  [--warmup N] [--idle-ms MS] [--s10-runs N] [--short] [--gate G1,G5,...]
//                  [--record-e2e DIR] [--pin-client CPU|none] [--pin-compositor CPU|none]
//                  [--git SHA] [--preset NAME] [--s9-ms MS] [--frames-out PATH]
//
// Processes. The orchestrator (this process) starts a headless compositor, then runs each
// (tier, scenario) as a window in a child process of its own — `--client-run` — because
// lx.app's runtime (the source table, the executor's UI strand, lx.ui's per-thread state) is
// one per process, and because the idle and allocation numbers must be the window's alone:
// a child's /proc/self/task counts nothing else. S10 starts `lumen-hello` itself.
//
// The compositor. `--compositor host` (the default) is this binary in the `--compositor-host`
// role: `lx::compositor::compositor` with exactly what `lumen-compositor SOCKET
// --virtual-outputs 1280x800 --no-control --ping-timeout 0` configures (a private socket, one
// 60 Hz virtual output, no card opened, the default configuration: LUMEN_CONFIG=0), under its
// own `run()` — its frame clock, render thread and CPU composite are the binary's — plus a
// command socket ($XDG_RUNTIME_DIR/SOCKET.bench) on which a window asks for what only the
// compositor can do: virtual pointer motion and buttons and keys, posted to the compositor's
// UI strand the way a libinput event arrives (`inject_pointer_motion` and friends run the
// seat path: window manager, router, serials, focus), and its toplevel record. The
// lumen-compositor binary takes no injected input or forced resize from outside (no
// virtual-pointer protocol; its control socket has a handful of actions), so
// `--compositor binary` runs it for S1 and S10 only. `--connect` uses WAYLAND_DISPLAY: S1, S10
// and S9 passive — the window records its frames while scripts/ui-bench.sh --e2e injects real
// input through the kernel and joins the timestamps (ADR-010 §8.5).
//
// Scenarios (§8.3):
//   S1   an empty window left idle (`--idle-ms`, 10 s): the client's context switches over every
//        thread (/proc/self/task/*/status), loop wakeups, timer expirations, socket reads,
//        frame callbacks requested, commits and GPU submits — G1 wants every one 0 after the
//        last frame (the idle window's own quit timer is the one wakeup subtracted).
//   S5   a 1 000-row list (lx.ui `list`, virtualised); the compositor moves the pointer one
//        row per refresh period: per-frame UI-thread time by stage, damage rects (≤ 2), layout
//        calls, commit → present.
//   S7   30 compositor-driven resizes 200 ms apart of a window of wrapped text — an
//        interactive resize (Super + right drag, the default binding), one configure per
//        motion: heap before/after (mallinfo2) and `operator new`, frame times against their
//        median, swapchain reallocations against the sizes seen — G11.
//   S9   a button hovered by 125 Hz injected motion for 3 s, the pointer crossing its edge
//        every 4th motion: injection time → the wp_presentation time of the first frame the
//        window started after it, median and p95, in refresh periods. Headless this is a proxy
//        for G6 (a timer-driven virtual output, no scanout); the real number comes from
//        `scripts/ui-bench.sh --e2e` on the KMS display.
//   S10  `lumen-hello --exit-after-first-present` ×`--s10-runs` per tier with an empty Lumen
//        cache (cold: no font matches, no pipeline cache) and with a warm one: exec → first
//        commit and → first presented (LUMEN_EXEC_NS taken right before execve) — G7. GL keeps
//        no program binaries on a driver with GL_NUM_PROGRAM_BINARY_FORMATS = 0: its warm row
//        is the font matches only, and says so.
//
// Every steady scenario (S1, S5, S9) counts `operator new` and `malloc` on the window's UI
// thread over its measured window (G5 wants `operator new` 0; libwayland's per-message
// mallocs are reported, net), and mallinfo2's in-use delta for the process.
//
// Tiers: cpu always; gl when /dev/dri/renderD128 exists; vulkan on whatever device lx.gfx finds
// — on a host with only lavapipe the rows are labelled software (driver "vulkan-software"):
// parity, never a gate or a baseline. The headless compositor names no main device, so every
// tier is forced (automatic would pick cpu); a forced tier the host cannot run falls back, and
// its rows are reported skipped.
//
// JSON: one array of §8.5 records, each with "mode": "client" (the headless S1–S4 records are
// unchanged and carry no mode), "compositor", the tier as "backend", "driver", per-stage
// distributions, counters, memory, "client" (scenario-specific) and "gates". `--gate LIST`
// makes the exit status 1 when one of those gates fails (the CTest `bench_ui_client`).
//
// LIVE COMPOSITOR RULE: the compositor this starts is headless (virtual outputs, no card) on a
// private socket (lumen-bench-<pid>), and is stopped on exit (SIGTERM, then SIGKILL); its
// children die with it (PR_SET_PDEATHSIG).

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.gfx;
import lx.scene;
import lx.ui;
import lx.app;
import lx.input;
import lx.compositor;
import lx.trace;

extern char** environ;

/// tests/bench_ui.cpp: the calling thread's `operator new` calls so far.
std::uint64_t bench_thread_new_calls() noexcept;

// ── malloc accounting, per thread ────────────────────────────────────────────────
//
// `operator new` is counted in bench_ui.cpp; these count every malloc/calloc/realloc (new's
// included) on the calling thread, so a window's UI thread can be told from its worker, the
// libwayland reader and a driver's threads. Initial-exec TLS: no allocation on first touch.

#if defined(__SANITIZE_ADDRESS__)
#define LUMEN_BENCH_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LUMEN_BENCH_SANITIZED 1
#endif
#endif

namespace {
__attribute__((tls_model("initial-exec"))) thread_local std::uint64_t t_malloc_calls = 0;
} // namespace

#if !defined(LUMEN_BENCH_SANITIZED)
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void __libc_free(void*);

void* malloc(size_t n) noexcept {
    ++t_malloc_calls;
    return __libc_malloc(n);
}
void* calloc(size_t n, size_t s) noexcept {
    ++t_malloc_calls;
    return __libc_calloc(n, s);
}
void* realloc(void* p, size_t n) noexcept {
    ++t_malloc_calls;
    return __libc_realloc(p, n);
}
void free(void* p) noexcept { __libc_free(p); }
}
#endif

namespace {

using u64 = std::uint64_t;

constexpr const char* k_app_id = "org.lumen.bench";
constexpr unsigned k_max_frames = 8192;
constexpr unsigned k_max_injections = 8192;

[[nodiscard]] u64 mono_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
}

void sleep_until_ns(u64 deadline) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(deadline / 1'000'000'000ull);
    ts.tv_nsec = static_cast<long>(deadline % 1'000'000'000ull);
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

[[nodiscard]] double load1() {
    double a = -1.0;
    if (std::FILE* f = std::fopen("/proc/loadavg", "r")) {
        if (std::fscanf(f, "%lf", &a) != 1)
            a = -1.0;
        std::fclose(f);
    }
    return a;
}

[[nodiscard]] long rss_anon_kb() {
    long kb = -1;
    if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, "RssAnon:", 8) == 0) {
                kb = std::strtol(line + 8, nullptr, 10);
                break;
            }
        std::fclose(f);
    }
    return kb;
}

/// Bytes the process holds from malloc: chunks in the arenas (uordblks) and chunks mmapped
/// on their own (hblkhd). Both: glibc raises its mmap threshold to the size of a freed mmapped
/// chunk, so the same buffer allocated again moves from hblkhd to uordblks — neither alone is
/// what the process holds.
[[nodiscard]] long long heap_in_use() {
    const struct mallinfo2 m = ::mallinfo2();
    return static_cast<long long>(m.uordblks + m.hblkhd);
}

/// What the host took from this thread and machine: the calling thread's time waiting on a
/// run queue (/proc/thread-self/schedstat) and the steal time of every vCPU (/proc/stat).
struct host_time {
    u64 run_delay_ns = 0;
    u64 steal_ms = 0;
    void take() {
        run_delay_ns = 0;
        steal_ms = 0;
        if (std::FILE* f = std::fopen("/proc/thread-self/schedstat", "r")) {
            unsigned long long on_cpu = 0, delay = 0;
            if (std::fscanf(f, "%llu %llu", &on_cpu, &delay) == 2)
                run_delay_ns = delay;
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen("/proc/stat", "r")) {
            unsigned long long v[8]{};
            if (std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                            &v[3], &v[4], &v[5], &v[6], &v[7]) == 8)
                steal_ms = v[7] * 1000ull / static_cast<unsigned long long>(::sysconf(_SC_CLK_TCK));
            std::fclose(f);
        }
    }
};

/// Nearest-rank percentiles of `ns`, in µs.
struct summary {
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
    unsigned samples = 0;
};

[[nodiscard]] summary summarize(std::vector<u64> ns) {
    summary s{};
    s.samples = static_cast<unsigned>(ns.size());
    if (ns.empty())
        return s;
    std::sort(ns.begin(), ns.end());
    const auto at = [&](double p) {
        auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(ns.size())));
        rank = std::clamp<std::size_t>(rank, 1, ns.size());
        return static_cast<double>(ns[rank - 1]) / 1000.0;
    };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    s.max = static_cast<double>(ns.back()) / 1000.0;
    return s;
}

// ── /proc/<pid>/task: context switches per thread ──

struct thread_row {
    int tid = 0;
    char comm[32]{};
    u64 voluntary = 0;
    u64 involuntary = 0;
};

struct thread_table {
    thread_row rows[64]{};
    unsigned count = 0;
    [[nodiscard]] const thread_row* find(int tid) const {
        for (unsigned i = 0; i < count; ++i)
            if (rows[i].tid == tid)
                return &rows[i];
        return nullptr;
    }
};

void read_threads(int pid, thread_table& out) {
    out.count = 0;
    char dir[64];
    if (pid > 0)
        std::snprintf(dir, sizeof dir, "/proc/%d/task", pid);
    else
        std::snprintf(dir, sizeof dir, "/proc/self/task");
    DIR* d = ::opendir(dir);
    if (!d)
        return;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || out.count >= std::size(out.rows))
            continue;
        thread_row& r = out.rows[out.count];
        r = {};
        r.tid = std::atoi(e->d_name);
        char path[128];
        std::snprintf(path, sizeof path, "%s/%s/status", dir, e->d_name);
        std::FILE* f = std::fopen(path, "r");
        if (!f)
            continue;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "Name:", 5) == 0) {
                const char* v = line + 5;
                while (*v == ' ' || *v == '\t')
                    ++v;
                std::snprintf(r.comm, sizeof r.comm, "%s", v);
                if (char* nl = std::strchr(r.comm, '\n'))
                    *nl = '\0';
            } else if (std::strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
                r.voluntary = std::strtoull(line + 24, nullptr, 10);
            } else if (std::strncmp(line, "nonvoluntary_ctxt_switches:", 27) == 0) {
                r.involuntary = std::strtoull(line + 27, nullptr, 10);
            }
        }
        std::fclose(f);
        ++out.count;
    }
    ::closedir(d);
}

/// Switches between two snapshots, summed over the threads alive in both (and new ones).
struct switch_delta {
    u64 voluntary = 0;
    u64 involuntary = 0;
    unsigned threads = 0;
    char detail[512]{}; ///< "comm:v+i …" for the threads that switched
};

[[nodiscard]] switch_delta diff_threads(const thread_table& a, const thread_table& b) {
    switch_delta d{};
    d.threads = b.count;
    std::size_t at = 0;
    for (unsigned i = 0; i < b.count; ++i) {
        const thread_row& r = b.rows[i];
        const thread_row* was = a.find(r.tid);
        const u64 v = r.voluntary - (was ? was->voluntary : 0);
        const u64 n = r.involuntary - (was ? was->involuntary : 0);
        d.voluntary += v;
        d.involuntary += n;
        if ((v || n) && at + 48 < sizeof d.detail) {
            const int w = std::snprintf(d.detail + at, sizeof d.detail - at, "%s%s:%llu+%llu",
                                        at ? " " : "", r.comm, static_cast<unsigned long long>(v),
                                        static_cast<unsigned long long>(n));
            if (w > 0)
                at += static_cast<std::size_t>(w);
        }
    }
    return d;
}

// ── JSON ──

void json_string(std::FILE* f, const char* s) {
    std::fputc('"', f);
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\')
            std::fputc('\\', f);
        if (static_cast<unsigned char>(*s) < 0x20)
            continue;
        std::fputc(*s, f);
    }
    std::fputc('"', f);
}

struct host_info {
    char kernel[128] = "unknown";
    char nodename[128] = "unknown";
    char machine[64] = "unknown";
    char cpu[160] = "unknown";
    unsigned cpus = 1;
};

[[nodiscard]] host_info read_host() {
    host_info h{};
    utsname u{};
    if (::uname(&u) == 0) {
        std::snprintf(h.kernel, sizeof h.kernel, "%s", u.release);
        std::snprintf(h.nodename, sizeof h.nodename, "%s", u.nodename);
        std::snprintf(h.machine, sizeof h.machine, "%s", u.machine);
    }
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    h.cpus = n > 0 ? static_cast<unsigned>(n) : 1u;
    if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, "model name", 10) == 0) {
                if (const char* colon = std::strchr(line, ':')) {
                    ++colon;
                    while (*colon == ' ' || *colon == '\t')
                        ++colon;
                    std::snprintf(h.cpu, sizeof h.cpu, "%s", colon);
                    if (char* nl = std::strchr(h.cpu, '\n'))
                        *nl = '\0';
                }
                break;
            }
        std::fclose(f);
    }
    return h;
}

void json_stage(std::FILE* f, const char* name, const summary& s, bool& first) {
    std::fprintf(f, "%s\n      \"%s\": {\"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f, "
                    "\"samples\": %u}",
                 first ? "" : ",", name, s.p50, s.p95, s.p99, s.max, s.samples);
    first = false;
}

[[nodiscard]] bool pin_to(int cpu) {
    if (cpu < 0)
        return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::sched_setaffinity(0, sizeof set, &set) == 0;
}

[[nodiscard]] std::string bench_socket_path(const char* socket) {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    return std::string(dir ? dir : "/tmp") + "/" + socket + ".bench";
}

void die_with_parent() {
    (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() == 1)
        ::_exit(1);
}

// ════════════════════════════════════════════════════════════════════════════════════
// The compositor host (`--compositor-host SOCKET READY_FD`)
// ════════════════════════════════════════════════════════════════════════════════════

lx::compositor::compositor* g_comp = nullptr;

void on_host_signal(int) {
    if (g_comp)
        g_comp->request_stop();
}

void post_ui(lx::runtime::task t) {
    for (int i = 0; i < 1000 && !g_comp->executor().post(lx::runtime::affinity::ui, t); ++i)
        ::usleep(1000);
}

void inject_motion_now(double x, double y, u64 t_us) {
    post_ui(lx::runtime::task{[x, y, t_us] {
        lx::input::pointer_motion_event m{};
        m.x = x;
        m.y = y;
        m.time_us = t_us;
        m.time_ms = static_cast<unsigned>(t_us / 1000);
        g_comp->inject_pointer_motion(m);
    }});
}

void inject_button_now(unsigned code, bool pressed, u64 t_us) {
    post_ui(lx::runtime::task{[code, pressed, t_us] {
        lx::input::pointer_button_event b{};
        b.which = code == 0x111 ? lx::input::button::right : lx::input::button::left;
        b.evdev_code = code;
        b.pressed = pressed;
        b.time_us = t_us;
        b.time_ms = static_cast<unsigned>(t_us / 1000);
        g_comp->inject_pointer_button(b);
    }});
}

void inject_key_now(unsigned code, bool pressed, u64 t_us) {
    post_ui(lx::runtime::task{[code, pressed, t_us] {
        lx::input::key_event k{};
        k.evdev_keycode = code;
        k.pressed = pressed;
        k.time_us = t_us;
        k.time_ms = static_cast<unsigned>(t_us / 1000);
        g_comp->inject_key(k);
    }});
}

/// A request answered on the compositor's UI thread.
struct ui_query {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    char app_id[64]{};
    char reply[256]{};
};

void answer_record(void* raw) {
    auto* q = static_cast<ui_query*>(raw);
    lx::rect2i g{};
    bool mapped = false;
    unsigned id = 0;
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, q->app_id) == 0 && r.id.id() >= id) {
            id = r.id.id();
            g = r.geometry;
            mapped = r.mapped;
        }
    });
    {
        std::lock_guard<std::mutex> hold{q->m};
        std::snprintf(q->reply, sizeof q->reply, "ok %d %d %d %d %d %u", mapped ? 1 : 0, g.x, g.y,
                      g.width, g.height, id);
        q->done = true;
    }
    q->cv.notify_one();
}

/// One injected event, recorded as scripts/inject-input.py records its writes.
struct injection {
    u64 t_us = 0;
    double x = 0, y = 0;
    unsigned kind = 0; ///< 0 motion, 1 button, 2 key
    unsigned code = 0;
    bool pressed = false;
    bool inside = false;
    const char* phase = "";
};

void write_injections(const char* path, const char* tag, const std::vector<injection>& recs) {
    if (!path || !*path || std::strcmp(path, "-") == 0)
        return;
    std::FILE* f = std::fopen(path, "w");
    if (!f)
        return;
    for (const injection& r : recs) {
        if (r.kind == 0)
            std::fprintf(f,
                         "{\"x\":%.2f,\"y\":%.2f,\"phase\":\"%s\",\"inside\":%s,\"kind\":\"motion\","
                         "\"t_us\":%llu,\"tag\":\"%s\"}\n",
                         r.x, r.y, r.phase, r.inside ? "true" : "false",
                         static_cast<unsigned long long>(r.t_us), tag);
        else if (r.kind == 1)
            std::fprintf(f, "{\"button\":\"%s\",\"pressed\":%s,\"kind\":\"button\",\"t_us\":%llu,\"tag\":\"%s\"}\n",
                         r.code == 0x111 ? "right" : "left", r.pressed ? "true" : "false",
                         static_cast<unsigned long long>(r.t_us), tag);
        else
            std::fprintf(f, "{\"code\":%u,\"pressed\":%s,\"kind\":\"key\",\"t_us\":%llu,\"tag\":\"%s\"}\n",
                         r.code, r.pressed ? "true" : "false", static_cast<unsigned long long>(r.t_us),
                         tag);
    }
    std::fclose(f);
}

[[nodiscard]] u64 mono_us() { return mono_ns() / 1000; }

/// Serves one connection: a command per message, a reply per command. Blocks with no
/// timeout (the compositor's idle numbers must not carry this thread's wakeups); `stop_fd`
/// becomes readable at shutdown.
void serve_connection(int fd, int stop_fd, const std::atomic<bool>& stop) {
    char buf[1024];
    std::vector<injection> recs;
    recs.reserve(k_max_injections);
    while (!stop.load(std::memory_order_acquire)) {
        pollfd pf[2] = {{fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
        if (::poll(pf, 2, -1) <= 0 || (pf[1].revents & POLLIN))
            continue;
        const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0)
            return;
        buf[n] = '\0';
        char cmd[32]{};
        char path[512]{};
        char reply[256] = "ok";
        (void)std::sscanf(buf, "%31s", cmd);
        recs.clear();
        if (std::strcmp(cmd, "record") == 0) {
            ui_query q{};
            (void)std::sscanf(buf, "%*s %63s", q.app_id);
            post_ui(lx::runtime::task{&answer_record, &q});
            std::unique_lock<std::mutex> hold{q.m};
            if (!q.cv.wait_for(hold, std::chrono::seconds(2), [&] { return q.done; }))
                std::snprintf(reply, sizeof reply, "fail timeout");
            else
                std::snprintf(reply, sizeof reply, "%s", q.reply);
        } else if (std::strcmp(cmd, "move") == 0) {
            double x = 0, y = 0;
            (void)std::sscanf(buf, "%*s %lf %lf", &x, &y);
            inject_motion_now(x, y, mono_us());
        } else if (std::strcmp(cmd, "rows") == 0) {
            // rows X Y0 DY N COUNT PERIOD_US PATH: the pointer one row further each period,
            // over N rows and back (a ping-pong), COUNT motions.
            double x = 0, y0 = 0, dy = 0;
            unsigned rows = 1, count = 0, period = 16667;
            (void)std::sscanf(buf, "%*s %lf %lf %lf %u %u %u %511s", &x, &y0, &dy, &rows, &count,
                              &period, path);
            rows = std::max(rows, 2u);
            count = std::min(count, k_max_injections);
            const u64 start = mono_ns() + 10'000'000ull;
            for (unsigned i = 0; i < count && !stop.load(); ++i) {
                sleep_until_ns(start + static_cast<u64>(i) * period * 1000ull);
                const unsigned cycle = 2 * (rows - 1);
                const unsigned k = i % cycle;
                const unsigned row = k < rows ? k : cycle - k;
                const double y = y0 + dy * row;
                const u64 t = mono_us();
                inject_motion_now(x, y, t);
                recs.push_back({t, x, y, 0, 0, false, false, "rows"});
            }
            write_injections(path, "S5", recs);
            std::snprintf(reply, sizeof reply, "done %zu", recs.size());
        } else if (std::strcmp(cmd, "hover") == 0) {
            // hover XIN YIN XOUT YOUT COUNT PERIOD_US TOGGLE PATH: COUNT motions, TOGGLE in a row
            // on each side of the button's edge, a pixel apart so each is a real motion.
            double xin = 0, yin = 0, xout = 0, yout = 0;
            unsigned count = 0, period = 8000, toggle = 4;
            (void)std::sscanf(buf, "%*s %lf %lf %lf %lf %u %u %u %511s", &xin, &yin, &xout, &yout,
                              &count, &period, &toggle, path);
            toggle = std::max(toggle, 1u);
            count = std::min(count, k_max_injections);
            const u64 start = mono_ns() + 10'000'000ull;
            for (unsigned i = 0; i < count && !stop.load(); ++i) {
                sleep_until_ns(start + static_cast<u64>(i) * period * 1000ull);
                const bool inside = (i / toggle) % 2 == 1;
                const double jitter = static_cast<double>(i % toggle);
                const double x = (inside ? xin : xout) + jitter;
                const double y = inside ? yin : yout;
                const u64 t = mono_us();
                inject_motion_now(x, y, t);
                recs.push_back({t, x, y, 0, 0, false, inside, "hover"});
            }
            write_injections(path, "S9", recs);
            std::snprintf(reply, sizeof reply, "done %zu", recs.size());
        } else if (std::strcmp(cmd, "storm") == 0) {
            // storm X Y DX DY COUNT INTERVAL_US PATH: Super + right button at (X, Y) — the
            // default binding's interactive resize from the nearest corner — then COUNT
            // motions of (DX, DY), INTERVAL_US apart, then release.
            double x = 0, y = 0, dx = 0, dy = 0;
            unsigned count = 0, interval = 200'000;
            (void)std::sscanf(buf, "%*s %lf %lf %lf %lf %u %u %511s", &x, &y, &dx, &dy, &count,
                              &interval, path);
            count = std::min(count, k_max_injections - 8);
            constexpr unsigned k_super = 125;
            constexpr unsigned k_right = 0x111;
            u64 t = mono_us();
            inject_motion_now(x, y, t);
            recs.push_back({t, x, y, 0, 0, false, false, "pre"});
            ::usleep(50'000);
            t = mono_us();
            inject_key_now(k_super, true, t);
            recs.push_back({t, x, y, 2, k_super, true, false, ""});
            ::usleep(30'000);
            t = mono_us();
            inject_button_now(k_right, true, t);
            recs.push_back({t, x, y, 1, k_right, true, false, ""});
            const u64 start = mono_ns() + 50'000'000ull;
            for (unsigned i = 1; i <= count && !stop.load(); ++i) {
                sleep_until_ns(start + static_cast<u64>(i - 1) * interval * 1000ull);
                const double px = x + dx * i;
                const double py = y + dy * i;
                t = mono_us();
                inject_motion_now(px, py, t);
                recs.push_back({t, px, py, 0, 0, false, false, "drag"});
            }
            sleep_until_ns(start + static_cast<u64>(count) * interval * 1000ull);
            t = mono_us();
            inject_button_now(k_right, false, t);
            recs.push_back({t, x, y, 1, k_right, false, false, ""});
            ::usleep(30'000);
            t = mono_us();
            inject_key_now(k_super, false, t);
            recs.push_back({t, x, y, 2, k_super, false, false, ""});
            write_injections(path, "S7", recs);
            std::snprintf(reply, sizeof reply, "done %zu", recs.size());
        } else {
            std::snprintf(reply, sizeof reply, "fail unknown command");
        }
        (void)::send(fd, reply, std::strlen(reply), MSG_NOSIGNAL);
    }
}

int host_main(const char* socket, int ready_fd, int pin) {
    die_with_parent();
    if (pin >= 0)
        (void)pin_to(pin);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    // The defaults, not this user's configuration: bindings, focus and placement as shipped.
    ::setenv("LUMEN_CONFIG", "0", 1);
    lx::compositor::config cfg{};
    cfg.socket_name = socket;
    cfg.target_fps = 60.0;
    cfg.present = lx::compositor::present_backend::cpu;
    cfg.virtual_outputs = "1280x800";
    cfg.ping_timeout_ms = 0;
    cfg.control_socket = false;
    lx::compositor::compositor comp{cfg};
    g_comp = &comp;
    struct sigaction sa{};
    sa.sa_handler = on_host_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    // No Vulkan driver in the compositor: its CPU path keeps a bookkeeping import of the GL
    // tier's dma-bufs (lavapipe cannot import vmwgfx buffers); the windows load their own.
    ::setenv("VK_LOADER_DRIVERS_DISABLE", "*", 1);
    auto started = comp.start();
    ::unsetenv("VK_LOADER_DRIVERS_DISABLE");
    if (!started) {
        std::fprintf(stderr, "lumen-ui-bench host: compositor start failed: %s\n",
                     lx::format_error(started.get_error()));
        return 1;
    }
    const std::string path = bench_socket_path(socket);
    ::unlink(path.c_str());
    const int listen_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    if (listen_fd < 0 || ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(listen_fd, 4) != 0) {
        std::fprintf(stderr, "lumen-ui-bench host: cannot listen on %s (%s)\n", path.c_str(),
                     std::strerror(errno));
        return 1;
    }
    std::atomic<bool> stop{false};
    int stop_pipe[2] = {-1, -1};
    if (::pipe2(stop_pipe, O_CLOEXEC) != 0)
        return 1;
    std::thread commands([&] {
        while (!stop.load(std::memory_order_acquire)) {
            pollfd pf[2] = {{listen_fd, POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
            if (::poll(pf, 2, -1) <= 0 || (pf[1].revents & POLLIN))
                continue;
            const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0)
                continue;
            serve_connection(fd, stop_pipe[0], stop);
            ::close(fd);
        }
    });
    if (ready_fd >= 0) {
        (void)::write(ready_fd, "R", 1);
        ::close(ready_fd);
    }
    const int code = comp.run();
    stop.store(true, std::memory_order_release);
    (void)::write(stop_pipe[1], "x", 1);
    commands.join();
    ::close(stop_pipe[0]);
    ::close(stop_pipe[1]);
    ::close(listen_fd);
    ::unlink(path.c_str());
    g_comp = nullptr;
    return code;
}

// ════════════════════════════════════════════════════════════════════════════════════
// A window (`--client-run SCENARIO TIER ...`)
// ════════════════════════════════════════════════════════════════════════════════════

struct run_args {
    const char* scenario = "S1";
    const char* tier = "cpu";
    const char* socket = nullptr; ///< null: WAYLAND_DISPLAY (`--connect`)
    const char* result = nullptr;
    const char* record_dir = nullptr;
    const char* frames_out = nullptr;
    const char* git = "";
    const char* preset = "release";
    const char* compositor = "host";
    unsigned frames = 600;
    unsigned warmup = 120;
    unsigned idle_ms = 10'000;
    unsigned s9_ms = 3'000;
    unsigned storm = 30;
    int host_pid = 0;
    int pin = -1;
};

[[nodiscard]] lx::ui::child s1_describe(lx::ui::build_context&) {
    using namespace lx::ui;
    return div(style{}.size_full().bg(tok::background), std::span<const child>{});
}

[[nodiscard]] lx::ui::child s5_row(void*, const lx::ui::list_row& row) {
    using namespace lx::ui;
    return list_item(frame_format("Row {}", row.index)).row(row);
}

[[nodiscard]] lx::ui::child s5_describe(lx::ui::build_context&) {
    using namespace lx::ui;
    const child kids[1] = {list(1000, &s5_row).layout(style{}.size_full()).name("rows")};
    return div(style{}.size_full().bg(tok::background), kids);
}

constexpr const char* k_s7_text[4] = {
    "Lumen draws this window with its own engine: one primitive stream of rounded quads, "
    "shadows and atlas sprites, emitted by retained nodes, ordered and batched once, and "
    "rendered on the CPU, with GLES or with Vulkan into a buffer the compositor samples in "
    "place.",
    "When the window changes size the compositor sends a configure; the window lays its text "
    "out again at the new width, answers with a buffer of that size in the very commit that "
    "acknowledges it, and draws nothing it did not have to.",
    "Every paragraph here wraps at the window's width. Narrow it and the lines grow in number; "
    "widen it and they shorten — the shaping results are kept, and only the line breaks are "
    "computed again.",
    "A resize storm is thirty of these in six seconds: the heap must stay flat, no frame may "
    "take more than twice the median, and the swapchain may allocate once per size it sees.",
};

[[nodiscard]] lx::ui::child s7_describe(lx::ui::build_context&) {
    using namespace lx::ui;
    const child kids[5] = {
        headline("Resize storm").size(headline_size::large),
        label(k_s7_text[0]),
        label(k_s7_text[1]).color(tok::text_muted),
        label(k_s7_text[2]),
        label(k_s7_text[3]).color(tok::text_muted),
    };
    const child body[1] = {div(style{}.flex_col().gap(sp::b16).p(sp::b24).size_full(), kids)};
    return div(style{}.size_full().bg(tok::background), body);
}

constexpr lx::ui::element_key k_s9_button = 0x5309'0001;

[[nodiscard]] lx::ui::child s9_describe(lx::ui::build_context&) {
    using namespace lx::ui;
    const child btn[1] = {button("Hover me")
                              .style(button_style::filled)
                              .size(button_size::large)
                              .full_width()
                              .key(k_s9_button)};
    const child mid[1] = {div(style{}.w(px(320.f)), btn)};
    return div(style{}.size_full().bg(tok::background).flex_col().items_center().justify_center(),
               mid);
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

/// The link to the compositor host's command socket.
struct host_link {
    int fd = -1;
    ~host_link() {
        if (fd >= 0)
            ::close(fd);
    }
    [[nodiscard]] bool connect(const char* socket) {
        const std::string path = bench_socket_path(socket);
        fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        return fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    }
    void send(const char* line) const { (void)::send(fd, line, std::strlen(line), MSG_NOSIGNAL); }
    [[nodiscard]] bool receive(char* out, std::size_t n, int timeout_ms) const {
        pollfd pf{fd, POLLIN, 0};
        if (::poll(&pf, 1, timeout_ms) <= 0)
            return false;
        const ssize_t got = ::recv(fd, out, n - 1, 0);
        if (got <= 0)
            return false;
        out[got] = '\0';
        return true;
    }
    [[nodiscard]] bool call(const char* line, char* out, std::size_t n) const {
        send(line);
        return receive(out, n, 5000);
    }
};

struct geometry {
    bool ok = false;
    bool mapped = false;
    lx::rect2i rect{};
};

[[nodiscard]] geometry query_geometry(const host_link& h) {
    geometry g{};
    char line[96];
    std::snprintf(line, sizeof line, "record %s", k_app_id);
    char reply[256];
    if (!h.call(line, reply, sizeof reply))
        return g;
    int mapped = 0;
    if (std::sscanf(reply, "ok %d %d %d %d %d", &mapped, &g.rect.x, &g.rect.y, &g.rect.width,
                    &g.rect.height) == 5) {
        g.ok = true;
        g.mapped = mapped != 0;
    }
    return g;
}

/// Runs the loop for `ms` with one quit timer and nothing else: what an idle window does.
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

/// Frames taken out of the window's 256-entry ring as they become final.
struct frame_log {
    std::vector<lx::app::frame_record> frames;
    std::uint64_t last_commit = 0;
    frame_log() { frames.reserve(k_max_frames); }
    /// Records committed at or after `since_ns` and older than `older_than_ns` (or all, 0).
    void harvest(const lx::app::window& w, u64 since_ns, u64 older_than_ns) {
        const unsigned n = w.frame_count();
        for (unsigned i = 0; i < n; ++i) {
            const lx::app::frame_record& r = w.frame_at(i);
            if (r.commit_id <= last_commit || r.commit_ns < since_ns)
                continue;
            if (older_than_ns && r.commit_ns >= older_than_ns)
                break;
            if (frames.size() < frames.capacity())
                frames.push_back(r);
            last_commit = r.commit_id;
        }
    }
};

/// Runs the loop until the host's reply to the command just sent arrives (or `timeout_ms`),
/// harvesting frames every second while it waits.
bool run_until_reply(lx::application& app, lx::app::window& w, const host_link& h, char* reply,
                     std::size_t n, int timeout_ms, frame_log* log = nullptr, u64 since_ns = 0) {
    lx::runtime::event_loop& loop = app.event_loop();
    struct ctx {
        lx::runtime::event_loop* loop;
        const host_link* h;
        char* reply;
        std::size_t n;
        bool got;
        const lx::app::window* w;
        frame_log* log;
        u64 since;
    } c{&loop, &h, reply, n, false, &w, log, since_ns};
    lx::runtime::fd_source answer{h.fd, lx::runtime::task{[p = &c] {
                                      if (p->h->receive(p->reply, p->n, 0)) {
                                          p->got = true;
                                          p->loop->quit(0);
                                      }
                                  }}};
    lx::runtime::timer_source harvest{1'000'000'000, lx::runtime::task{[p = &c] {
                                          if (p->log)
                                              p->log->harvest(*p->w, p->since, mono_ns() - 150'000'000ull);
                                      }}};
    lx::runtime::timer_source limit{0, lx::runtime::task{[&loop] { loop.quit(0); }}, false};
    limit.schedule_at(loop.now() + static_cast<long long>(timeout_ms) * 1'000'000);
    loop.add_source(&answer);
    loop.add_source(&limit);
    if (log)
        loop.add_source(&harvest);
    (void)loop.run();
    if (log)
        loop.remove_source(&harvest);
    loop.remove_source(&limit);
    loop.remove_source(&answer);
    return c.got;
}

/// One measurement window on the UI thread: allocations, heap, the loop's own counters.
struct heap_mark {
    u64 news = 0;
    u64 mallocs = 0;
    long long heap = 0;
    void take() {
        heap = heap_in_use();
        mallocs = t_malloc_calls;
        news = bench_thread_new_calls();
    }
};

struct heap_delta {
    u64 news = 0;
    u64 mallocs = 0;
    long long heap = 0;
};

[[nodiscard]] heap_delta since(const heap_mark& a) {
    const u64 news = bench_thread_new_calls();
    const u64 mallocs = t_malloc_calls;
    const long long heap = heap_in_use();
    return {news - a.news, mallocs - a.mallocs, heap - a.heap};
}

struct gate_row {
    const char* id = "";
    const char* verdict = "pass";
    char detail[640]{};
};

/// What a scenario writes: the shared fields; the scenario adds its stages and blocks.
struct result_writer {
    std::FILE* f = nullptr;
    gate_row gates[8]{};
    unsigned gate_count = 0;

    void gate(const char* id, const char* verdict, const char* fmt, ...) __attribute__((format(printf, 4, 5))) {
        if (gate_count >= std::size(gates))
            return;
        gate_row& g = gates[gate_count++];
        g.id = id;
        g.verdict = verdict;
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(g.detail, sizeof g.detail, fmt, ap);
        va_end(ap);
    }
};

[[nodiscard]] const char* driver_of(const lx::app::window& w, const char* tier) {
    if (w.tier() == lx::gfx::client_tier::vulkan && w.gpu().software())
        return "vulkan-software";
    if (w.tier() == lx::gfx::client_tier::gl && w.gpu().software())
        return "gl-software";
    return tier;
}

void write_header(std::FILE* f, const run_args& a, const lx::app::window& w, const char* title,
                  unsigned frames, double load_before, double load_after) {
    const host_info host = read_host();
    std::fputs("  {\n    \"host\": {\"kernel\": ", f);
    json_string(f, host.kernel);
    std::fprintf(f, ", \"cpus\": %u, \"cpu\": ", host.cpus);
    json_string(f, host.cpu);
    std::fputs(", \"arch\": ", f);
    json_string(f, host.machine);
    std::fputs(", \"hostname\": ", f);
    json_string(f, host.nodename);
    std::fputs("},\n    \"driver\": ", f);
    json_string(f, driver_of(w, a.tier));
    std::fputs(",\n    \"git\": ", f);
    json_string(f, a.git);
    std::fputs(",\n    \"preset\": ", f);
    json_string(f, a.preset);
    std::fprintf(f, ",\n    \"mode\": \"client\",\n    \"compositor\": \"%s\",\n    \"scenario\": \"%s\","
                    "\n    \"title\": ",
                 a.compositor, a.scenario);
    json_string(f, title);
    std::fputs(",\n    \"backend\": ", f);
    json_string(f, a.tier);
    std::fprintf(f, ",\n    \"frames\": %u,\n    \"warmup\": %u,\n    \"load\": {\"before\": %.2f, "
                    "\"after\": %.2f},\n",
                 frames, a.warmup, load_before, load_after);
    std::fprintf(f, "    \"window\": {\"tier\": \"%s\", \"software\": %s, \"gpu\": ",
                 lx::gfx::client_tier_name(w.tier()), w.gpu().software() ? "true" : "false");
    json_string(f, w.gpu().name());
    std::fprintf(f, ", \"logical\": [%d, %d], \"buffer\": [%d, %d], \"scale\": %.3f, "
                    "\"explicit_sync\": %s, \"images\": %u},\n",
                 w.logical_size().width, w.logical_size().height, w.buffer_size().width,
                 w.buffer_size().height, static_cast<double>(w.scale()),
                 w.swapchain().explicit_sync() ? "true" : "false", w.swapchain().image_count());
}

void write_gates(std::FILE* f, const result_writer& rw) {
    std::fputs("    \"gates\": {", f);
    for (unsigned i = 0; i < rw.gate_count; ++i)
        std::fprintf(f, "%s\"%s\": \"%s\"", i ? ", " : "", rw.gates[i].id, rw.gates[i].verdict);
    std::fputs("},\n    \"gate_details\": {", f);
    for (unsigned i = 0; i < rw.gate_count; ++i) {
        std::fprintf(f, "%s\"%s\": ", i ? ", " : "", rw.gates[i].id);
        json_string(f, rw.gates[i].detail);
    }
    std::fputs("}\n  }\n", f);
}

void print_gates(const run_args& a, const result_writer& rw) {
    for (unsigned i = 0; i < rw.gate_count; ++i)
        std::printf("  %s [%s] %s %s — %s\n", a.scenario, a.tier, rw.gates[i].id,
                    std::strcmp(rw.gates[i].verdict, "fail") == 0 ? "FAIL" : rw.gates[i].verdict,
                    rw.gates[i].detail);
    std::fflush(stdout);
}

void write_skip(const run_args& a, const char* why) {
    std::printf("  %s [%s] skipped — %s\n", a.scenario, a.tier, why);
    if (!a.result)
        return;
    std::FILE* f = std::fopen(a.result, "w");
    if (!f)
        return;
    std::fprintf(f, "  {\"mode\": \"client\", \"scenario\": \"%s\", \"backend\": \"%s\", \"skipped\": ",
                 a.scenario, a.tier);
    json_string(f, why);
    std::fputs("}\n", f);
    std::fclose(f);
}

/// The frame stages of `frames` (only those with a commit): §8.1's attribution.
void write_frame_stages(std::FILE* f, const std::vector<lx::app::frame_record>& frames, bool& first) {
    std::vector<u64> frame, frame_cpu, input, geometry, acquire, tick, describe, reconcile, layout, emit,
        order, render, present, c2p;
    for (const lx::app::frame_record& r : frames) {
        if (r.commit_ns >= r.start_ns && r.start_ns)
            frame.push_back(r.commit_ns - r.start_ns);
        frame_cpu.push_back(r.cpu_ns);
        input.push_back(r.input_ns);
        geometry.push_back(r.geometry_ns);
        acquire.push_back(r.acquire_ns);
        tick.push_back(r.tick_ns);
        describe.push_back(r.describe_ns);
        reconcile.push_back(r.reconcile_ns);
        layout.push_back(r.layout_ns);
        emit.push_back(r.emit_ns);
        order.push_back(r.order_ns);
        render.push_back(r.render_ns);
        present.push_back(r.present_ns);
        if (r.presented && r.presented_ns > r.commit_ns)
            c2p.push_back(r.presented_ns - r.commit_ns);
    }
    json_stage(f, "frame", summarize(frame), first);
    json_stage(f, "frame_cpu", summarize(frame_cpu), first);
    json_stage(f, "input", summarize(input), first);
    json_stage(f, "geometry", summarize(geometry), first);
    json_stage(f, "acquire", summarize(acquire), first);
    json_stage(f, "tick", summarize(tick), first);
    json_stage(f, "describe", summarize(describe), first);
    json_stage(f, "reconcile", summarize(reconcile), first);
    json_stage(f, "layout", summarize(layout), first);
    json_stage(f, "emit", summarize(emit), first);
    json_stage(f, "order", summarize(order), first);
    json_stage(f, "render", summarize(render), first);
    json_stage(f, "present", summarize(present), first);
    json_stage(f, "commit_to_present", summarize(c2p), first);
}

void print_stage(const char* name, const summary& s) {
    if (s.samples)
        std::printf("    %-18s %6u  p50 %9.1f  p95 %9.1f  max %9.1f us\n", name, s.samples, s.p50,
                    s.p95, s.max);
}

void print_frames(const std::vector<lx::app::frame_record>& frames) {
    std::vector<u64> frame, cpu, tick, render, c2p;
    for (const lx::app::frame_record& r : frames) {
        if (r.commit_ns >= r.start_ns && r.start_ns)
            frame.push_back(r.commit_ns - r.start_ns);
        cpu.push_back(r.cpu_ns);
        tick.push_back(r.tick_ns);
        render.push_back(r.render_ns);
        if (r.presented && r.presented_ns > r.commit_ns)
            c2p.push_back(r.presented_ns - r.commit_ns);
    }
    print_stage("frame", summarize(frame));
    print_stage("frame_cpu", summarize(cpu));
    print_stage("tick", summarize(tick));
    print_stage("render", summarize(render));
    print_stage("commit_to_present", summarize(c2p));
}

[[nodiscard]] u64 refresh_ns_of(const std::vector<lx::app::frame_record>& frames) {
    for (const lx::app::frame_record& r : frames)
        if (r.refresh_ns)
            return r.refresh_ns;
    return 16'666'667;
}

// ── the host's noise floor ──

volatile u64 g_spin_sink = 0;

/// A fixed integer workload of `iterations` (an LCG: no memory traffic, no syscalls).
void spin(u64 iterations) {
    u64 x = iterations;
    for (u64 k = 0; k < iterations; ++k)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    g_spin_sink = x;
}

struct control_result {
    summary wall{};
    unsigned over = 0;
    bool jank = false;
};

/// The same cadence as a measured scenario with a fixed CPU workload of `work_ns` per "frame"
/// instead of a frame: what this host does to a job whose cost never changes. Wall time per
/// job, `count` jobs `gap_us` apart.
[[nodiscard]] control_result run_control(u64 work_ns, unsigned count, unsigned gap_us) {
    // Calibrate: iterations per ns, the best of five 1 ms probes.
    double per_ns = 0;
    for (int i = 0; i < 5; ++i) {
        const u64 t0 = mono_ns();
        spin(1'000'000);
        const u64 dt = mono_ns() - t0;
        per_ns = std::max(per_ns, 1'000'000.0 / static_cast<double>(std::max<u64>(dt, 1)));
    }
    const u64 iterations = static_cast<u64>(per_ns * static_cast<double>(work_ns));
    std::vector<u64> wall;
    wall.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        ::usleep(gap_us);
        const u64 t0 = mono_ns();
        spin(iterations);
        wall.push_back(mono_ns() - t0);
    }
    control_result r{};
    r.wall = summarize(wall);
    for (u64 t : wall)
        r.over += static_cast<double>(t) / 1000.0 > 2.0 * r.wall.p50 ? 1u : 0u;
    r.jank = r.wall.max > 2.0 * r.wall.p50;
    return r;
}

// ── S9's join: the same rule scripts/ui-bench.sh --e2e applies to recorded logs ──

struct motion_rec {
    u64 t_us = 0;
    bool inside = false;
};

void read_motions(const char* path, std::vector<motion_rec>& out) {
    out.clear();
    std::FILE* f = std::fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (!std::strstr(line, "\"kind\":\"motion\""))
            continue;
        const char* t = std::strstr(line, "\"t_us\":");
        if (!t)
            continue;
        motion_rec m{};
        m.t_us = std::strtoull(t + 7, nullptr, 10);
        m.inside = std::strstr(line, "\"inside\":true") != nullptr;
        out.push_back(m);
    }
    std::fclose(f);
}

struct latency_sample {
    u64 t_us = 0;
    u64 to_start_ns = 0;
    u64 to_commit_ns = 0;
    u64 to_present_ns = 0;
};

/// Each hover transition (a motion whose `inside` differs from the one before) → the first
/// frame that started after it was injected; a sample when that frame was presented within
/// 500 ms. Frames between transitions do not exist (a motion that changes no hover state
/// draws nothing), so the first frame started after the motion is the one that shows it.
[[nodiscard]] std::vector<latency_sample> join_hover(const std::vector<motion_rec>& motions,
                                                     const std::vector<lx::app::frame_record>& frames,
                                                     unsigned& transitions) {
    std::vector<latency_sample> out;
    transitions = 0;
    for (std::size_t i = 1; i < motions.size(); ++i) {
        if (motions[i].inside == motions[i - 1].inside)
            continue;
        ++transitions;
        const u64 t_ns = motions[i].t_us * 1000ull;
        for (const lx::app::frame_record& r : frames) {
            if (r.start_ns < t_ns)
                continue;
            if (r.presented && r.presented_ns >= t_ns && r.presented_ns - t_ns < 500'000'000ull)
                out.push_back({motions[i].t_us, r.start_ns - t_ns, r.commit_ns - t_ns,
                               r.presented_ns - t_ns});
            break;
        }
    }
    return out;
}

void write_frames_json(const char* path, const lx::app::window& w,
                       const std::vector<lx::app::frame_record>& frames, lx::rect2i button,
                       lx::rect2i window_global) {
    std::FILE* f = std::fopen(path, "w");
    if (!f)
        return;
    std::fprintf(f, "{\"app_id\": \"%s\", \"tier\": \"%s\", \"logical\": [%d, %d], \"scale\": %.3f, "
                    "\"button\": [%d, %d, %d, %d], \"window\": [%d, %d, %d, %d], \"frames\": [",
                 k_app_id, lx::gfx::client_tier_name(w.tier()), w.logical_size().width,
                 w.logical_size().height, static_cast<double>(w.scale()), button.x, button.y,
                 button.width, button.height, window_global.x, window_global.y,
                 window_global.width, window_global.height);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const lx::app::frame_record& r = frames[i];
        std::fprintf(f,
                     "%s\n  {\"commit_id\": %llu, \"start_ns\": %llu, \"commit_ns\": %llu, "
                     "\"presented_ns\": %llu, \"refresh_ns\": %u, \"presented\": %s, "
                     "\"damage_rects\": %u, \"damage_px\": %u, \"size\": [%u, %u], \"cpu_ns\": %u, \"input_ns\": %u, "
                     "\"geometry_ns\": %u, \"acquire_ns\": %u, "
                     "\"tick_ns\": %u, \"layout_ns\": %u, \"emit_ns\": %u, \"render_ns\": %u, "
                     "\"present_ns\": %u}",
                     i ? "," : "", static_cast<unsigned long long>(r.commit_id),
                     static_cast<unsigned long long>(r.start_ns),
                     static_cast<unsigned long long>(r.commit_ns),
                     static_cast<unsigned long long>(r.presented_ns), r.refresh_ns,
                     r.presented ? "true" : "false", static_cast<unsigned>(r.damage_rects),
                     r.damage_px, static_cast<unsigned>(r.width), static_cast<unsigned>(r.height),
                     r.cpu_ns, r.input_ns,
                     r.geometry_ns, r.acquire_ns, r.tick_ns, r.layout_ns, r.emit_ns, r.render_ns,
                     r.present_ns);
    }
    std::fputs("\n]}\n", f);
    std::fclose(f);
}

// ── the scenarios ──

struct window_setup {
    const char* title;
    lx::size2i size;
    lx::ui::describe_fn describe;
};

[[nodiscard]] window_setup setup_of(const char* scenario) {
    if (std::strcmp(scenario, "S5") == 0)
        return {"lumen-ui-bench S5: 1 000-row list, hover one row per frame", {400, 736}, &s5_describe};
    if (std::strcmp(scenario, "S7") == 0)
        return {"lumen-ui-bench S7: resize storm of a text window", {640, 480}, &s7_describe};
    if (std::strcmp(scenario, "S9") == 0)
        return {"lumen-ui-bench S9: hover a button at 125 Hz", {640, 480}, &s9_describe};
    return {"lumen-ui-bench S1: empty window, idle", {640, 480}, &s1_describe};
}

int run_s1(const run_args& a, lx::application& app, lx::app::window& w, std::FILE* f) {
    run_for(app, 300); // the last frame's callback and presentation feedback come back
    const double load_before = load1();
    lx::runtime::event_loop& loop = app.event_loop();
    lx::runtime::timer_source quit{0, lx::runtime::task{[&loop] { loop.quit(0); }}, false};
    thread_table threads_a{}, threads_b{}, comp_a{}, comp_b{};
    read_threads(0, threads_a);
    if (a.host_pid > 0)
        read_threads(a.host_pid, comp_a);
    const lx::app::window_stats ws_a = w.stats();
    const lx::app::app_stats as_a = app.stats();
    const u64 wake_a = loop.wakeups();
    const unsigned long long reads_a = app.display_reads();
    const u64 presents_a = w.swapchain().stats().presents;
    quit.schedule_at(loop.now() + static_cast<long long>(a.idle_ms) * 1'000'000);
    loop.add_source(&quit);
    heap_mark mark{};
    mark.take();
    const u64 t0 = mono_ns();
    (void)loop.run();
    const u64 t1 = mono_ns();
    const heap_delta heap = since(mark);
    loop.remove_source(&quit);
    const u64 wake_b = loop.wakeups();
    const lx::app::window_stats ws_b = w.stats();
    const lx::app::app_stats as_b = app.stats();
    const unsigned long long reads_b = app.display_reads();
    const u64 presents_b = w.swapchain().stats().presents;
    read_threads(0, threads_b);
    if (a.host_pid > 0)
        read_threads(a.host_pid, comp_b);
    const double load_after = load1();
    const switch_delta sw = diff_threads(threads_a, threads_b);
    const switch_delta csw = diff_threads(comp_a, comp_b);
    const double seconds = static_cast<double>(t1 - t0) / 1e9;

    // The quit timer is the one wakeup the measurement itself causes: its expiry wakes the loop
    // once (one voluntary switch of the UI thread back to running).
    const u64 loop_wakeups = wake_b - wake_a;
    const u64 own = 1;
    const u64 wakeups = (loop_wakeups > own ? loop_wakeups - own : 0);
    const u64 switches = sw.voluntary + sw.involuntary;
    const u64 switches_net = switches > own ? switches - own : 0;
    const u64 frame_requests = ws_b.frame_requests - ws_a.frame_requests;
    const u64 commits = ws_b.commits - ws_a.commits;
    const u64 callbacks = ws_b.frame_callbacks - ws_a.frame_callbacks;
    const u64 submits = presents_b - presents_a;
    const u64 timers = as_b.timer_expirations - as_a.timer_expirations;
    const unsigned long long reads = reads_b - reads_a;

    result_writer rw{};
    const bool g1 = wakeups == 0 && switches_net == 0 && frame_requests == 0 && commits == 0 &&
                    submits == 0 && timers == 0 && reads == 0;
    rw.gate("G1", g1 ? "pass" : "fail",
            "%.1f s idle: %llu wakeups (%llu loop wakeups less the quit timer's), %.2f context "
            "switches/s over %u threads, %llu frame callbacks requested, %llu commits, %llu GPU "
            "submits, %llu timer expirations, %llu socket reads",
            seconds, static_cast<unsigned long long>(switches_net),
            static_cast<unsigned long long>(wakeups), static_cast<double>(switches_net) / seconds,
            sw.threads, static_cast<unsigned long long>(frame_requests),
            static_cast<unsigned long long>(commits), static_cast<unsigned long long>(submits),
            static_cast<unsigned long long>(timers), reads);
    rw.gate("G5", heap.news == 0 ? "pass" : "fail",
            "%llu operator new on the UI thread over the idle window",
            static_cast<unsigned long long>(heap.news));

    write_header(f, a, w, setup_of("S1").title, 0, load_before, load_after);
    std::fputs("    \"stages\": {},\n", f);
    std::fprintf(f,
                 "    \"counters\": {\"idle_ms\": %.1f, \"loop_wakeups\": %llu, \"wakeups\": %llu, "
                 "\"wakeups_per_s\": %.3f, \"voluntary_switches\": %llu, \"involuntary_switches\": "
                 "%llu, \"threads\": %u, \"frame_requests\": %llu, \"frame_callbacks\": %llu, "
                 "\"commits\": %llu, \"gpu_submits\": %llu, \"timer_expirations\": %llu, "
                 "\"socket_reads\": %llu, \"compositor_switches\": %llu, \"compositor_threads\": "
                 "%u},\n",
                 seconds * 1000.0, static_cast<unsigned long long>(loop_wakeups),
                 static_cast<unsigned long long>(switches_net),
                 static_cast<double>(switches_net) / seconds,
                 static_cast<unsigned long long>(sw.voluntary),
                 static_cast<unsigned long long>(sw.involuntary), sw.threads,
                 static_cast<unsigned long long>(frame_requests),
                 static_cast<unsigned long long>(callbacks), static_cast<unsigned long long>(commits),
                 static_cast<unsigned long long>(submits), static_cast<unsigned long long>(timers),
                 reads, static_cast<unsigned long long>(csw.voluntary + csw.involuntary),
                 csw.threads);
    std::fprintf(f,
                 "    \"memory\": {\"allocations\": %llu, \"allocations_per_steady_frame\": 0, "
                 "\"malloc_calls\": %llu, \"mallinfo2_in_use_delta\": %lld, \"rss_anon_kb\": "
                 "%ld},\n",
                 static_cast<unsigned long long>(heap.news),
                 static_cast<unsigned long long>(heap.mallocs), heap.heap, rss_anon_kb());
    std::fputs("    \"client\": {\"switch_detail\": ", f);
    json_string(f, sw.detail);
    std::fputs(", \"compositor_switch_detail\": ", f);
    json_string(f, csw.detail);
    std::fputs("},\n", f);
    write_gates(f, rw);
    std::printf("  S1 [%s] idle %.1f s: client switches %llu (%s), compositor %llu (%s)\n", a.tier,
                seconds, static_cast<unsigned long long>(switches), sw.detail,
                static_cast<unsigned long long>(csw.voluntary + csw.involuntary), csw.detail);
    print_gates(a, rw);
    return 0;
}

int run_s5(const run_args& a, lx::application& app, lx::app::window& w, std::FILE* f) {
    host_link h{};
    if (!h.connect(a.socket)) {
        write_skip(a, "no compositor command socket (S5 needs injected input: --compositor host)");
        return 0;
    }
    const geometry g = query_geometry(h);
    lx::ui::ui_node* list = find_type(w.root().tree(), lx::ui::list_node::static_type());
    if (!g.ok || !g.mapped || !list) {
        write_skip(a, "the window is not mapped, or has no list");
        return 1;
    }
    const lx::rect2i lb = lx::ui::surface_bounds(*list);
    const int row_h = static_cast<lx::ui::list_node*>(list)->row_height();
    const unsigned rows =
        std::clamp<unsigned>(static_cast<unsigned>(std::max(lb.height / std::max(row_h, 1), 3)) - 1, 2u, 28u);
    const double x = g.rect.x + lb.x + lb.width / 2.0;
    const double y0 = g.rect.y + lb.y + row_h / 2.0;
    char cmd[256];
    char reply[128];
    // Warm-up: the rows' hover paths, the atlas, the first allocations of every buffer.
    std::snprintf(cmd, sizeof cmd, "rows %.1f %.1f %d %u %u 16667 -", x, y0, row_h, rows, a.warmup);
    h.send(cmd);
    (void)run_until_reply(app, w, h, reply, sizeof reply, static_cast<int>(a.warmup * 17 + 3000));
    (void)settle(app, w);
    frame_log log{};
    const lx::app::window_stats ws_a = w.stats();
    const double load_before = load1();
    heap_mark mark{};
    mark.take();
    const u64 t0 = mono_ns();
    std::snprintf(cmd, sizeof cmd, "rows %.1f %.1f %d %u %u 16667 -", x, y0, row_h, rows, a.frames);
    h.send(cmd);
    const bool done = run_until_reply(app, w, h, reply, sizeof reply,
                                      static_cast<int>(a.frames * 17 + 5000), &log, t0);
    const heap_delta heap = since(mark);
    const u64 t1 = mono_ns();
    (void)settle(app, w);
    run_for(app, 150);
    log.harvest(w, t0, 0);
    const double load_after = load1();
    const lx::app::window_stats ws_b = w.stats();
    if (!done) {
        write_skip(a, "the compositor did not finish the row motions");
        return 1;
    }
    unsigned max_rects = 0, max_measures = 0, max_arranges = 0, presented = 0;
    for (const lx::app::frame_record& r : log.frames) {
        max_rects = std::max<unsigned>(max_rects, r.damage_rects);
        max_measures = std::max(max_measures, r.measures);
        max_arranges = std::max(max_arranges, r.arranges);
        presented += r.presented ? 1u : 0u;
    }
    const unsigned frames = static_cast<unsigned>(log.frames.size());
    result_writer rw{};
    rw.gate("G5", heap.news == 0 ? "pass" : "fail",
            "%llu operator new on the UI thread over %u hover frames (%llu malloc, net heap %+lld B)",
            static_cast<unsigned long long>(heap.news), frames,
            static_cast<unsigned long long>(heap.mallocs), heap.heap);
    rw.gate("damage", max_rects <= 2 && frames > 0 ? "pass" : "fail",
            "damage ≤ %u rect(s) per frame over %u frames", max_rects, frames);

    write_header(f, a, w, setup_of("S5").title, frames, load_before, load_after);
    std::fputs("    \"stages\": {", f);
    bool first = true;
    write_frame_stages(f, log.frames, first);
    std::fputs("\n    },\n", f);
    std::fprintf(f,
                 "    \"counters\": {\"motions\": %u, \"frames\": %u, \"presented\": %u, "
                 "\"frame_requests\": %llu, \"no_image\": %llu, \"damage_rects_max\": %u, "
                 "\"measures_max\": %u, \"arranges_max\": %u, \"rows\": %u, \"row_height\": %d, "
                 "\"seconds\": %.2f},\n",
                 a.frames, frames, presented,
                 static_cast<unsigned long long>(ws_b.frame_requests - ws_a.frame_requests),
                 static_cast<unsigned long long>(ws_b.no_image - ws_a.no_image), max_rects,
                 max_measures, max_arranges, rows, row_h, static_cast<double>(t1 - t0) / 1e9);
    std::fprintf(f,
                 "    \"memory\": {\"allocations\": %llu, \"allocations_per_steady_frame\": %.6f, "
                 "\"malloc_calls\": %llu, \"malloc_per_frame\": %.2f, \"mallinfo2_in_use_delta\": "
                 "%lld, \"rss_anon_kb\": %ld},\n",
                 static_cast<unsigned long long>(heap.news),
                 frames ? static_cast<double>(heap.news) / frames : 0.0,
                 static_cast<unsigned long long>(heap.mallocs),
                 frames ? static_cast<double>(heap.mallocs) / frames : 0.0, heap.heap,
                 rss_anon_kb());
    std::fputs("    \"client\": {},\n", f);
    write_gates(f, rw);
    if (a.record_dir) {
        const std::string frames_path = std::string(a.record_dir) + "/s5-frames.json";
        write_frames_json(frames_path.c_str(), w, log.frames, {}, g.rect);
    }
    std::printf("  S5 [%s] %u frames for %u row moves, damage ≤ %u rects, measures ≤ %u, arranges ≤ %u\n",
                a.tier, frames, a.frames, max_rects, max_measures, max_arranges);
    print_frames(log.frames);
    print_gates(a, rw);
    return 0;
}

int run_s7(const run_args& a, lx::application& app, lx::app::window& w, std::FILE* f) {
    host_link h{};
    if (!h.connect(a.socket)) {
        write_skip(a, "no compositor command socket (S7 needs a compositor-driven resize: --compositor host)");
        return 0;
    }
    const geometry g = query_geometry(h);
    if (!g.ok || !g.mapped) {
        write_skip(a, "the window is not mapped");
        return 1;
    }
    run_for(app, 200);
    (void)settle(app, w);
    // Steady before the storm: what the heap holds at the starting size.
    const double load_before = load1();
    const lx::app::window_stats ws_a = w.stats();
    const lx::size2i size_a = w.buffer_size();
    frame_log log{}; // its reserve before the mark: the bench's own memory is not the window's
    host_time host_a{};
    host_a.take();
    heap_mark mark{};
    mark.take();
    const u64 t0 = mono_ns();
    char cmd[256];
    char reply[128];
    const double px = g.rect.x + g.rect.width * 0.75;
    const double py = g.rect.y + g.rect.height * 0.75;
    std::snprintf(cmd, sizeof cmd, "storm %.1f %.1f 8 6 %u 200000 -", px, py, a.storm);
    h.send(cmd);
    const bool done = run_until_reply(app, w, h, reply, sizeof reply,
                                      static_cast<int>(a.storm * 200 + 5000), &log, t0);
    (void)settle(app, w);
    run_for(app, 300);
    (void)settle(app, w);
    log.harvest(w, t0, 0);
    const heap_delta heap = since(mark);
    host_time host_b{};
    host_b.take();
    const double load_after = load1();
    const lx::app::window_stats ws_b = w.stats();
    if (!done) {
        write_skip(a, "the compositor did not finish the resize storm");
        return 1;
    }
    // Sizes the frames had, frame times (wall and the UI thread's CPU), their medians.
    std::vector<u64> times, cpu_times;
    unsigned sizes = 0;
    std::uint16_t last_w = static_cast<std::uint16_t>(size_a.width);
    std::uint16_t last_h = static_cast<std::uint16_t>(size_a.height);
    for (const lx::app::frame_record& r : log.frames) {
        if (r.commit_ns >= r.start_ns && r.start_ns)
            times.push_back(r.commit_ns - r.start_ns);
        cpu_times.push_back(r.cpu_ns);
        if (r.width != last_w || r.height != last_h) {
            ++sizes;
            last_w = r.width;
            last_h = r.height;
        }
    }
    const summary st = summarize(times);
    const summary sc = summarize(cpu_times);
    const u64 reallocations = ws_b.reallocations - ws_a.reallocations;
    const u64 configures = ws_b.configures - ws_a.configures;
    const bool jank = st.samples > 0 && st.max > 2.0 * st.p50;
    const bool cpu_jank = sc.samples > 0 && sc.max > 2.0 * sc.p50;
    unsigned over = 0, cpu_over = 0;
    for (u64 t : times)
        over += static_cast<double>(t) / 1000.0 > 2.0 * st.p50 ? 1u : 0u;
    for (u64 t : cpu_times)
        cpu_over += static_cast<double>(t) / 1000.0 > 2.0 * sc.p50 ? 1u : 0u;
    const double run_delay_ms = static_cast<double>(host_b.run_delay_ns - host_a.run_delay_ns) / 1e6;
    const u64 steal_ms = host_b.steal_ms - host_a.steal_ms;
    // The noise floor: the storm's cadence with a fixed job of the median frame's CPU time.
    const control_result control =
        run_control(static_cast<u64>(sc.p50 * 1000.0), a.storm, 200'000);
    // Heap flat: what the process holds after the storm and a settle, against before it.
    // 64 KiB is the tolerance for allocator slack (a size class's partial page), not for growth
    // proportional to the resizes: 30 resizes of a leak would be far above it.
    constexpr long long k_flat_bytes = 64 * 1024;
    const bool flat = heap.heap <= k_flat_bytes;
    const bool realloc_ok = reallocations <= sizes;
    result_writer rw{};
    // The gate is the wall clock's (§3: no frame over 2x the median). When it misses and a
    // job whose cost never changes misses the same way at the same cadence on the same CPU
    // (`run_control`), the host cannot meet the rule for anything: reported "jitter", which is
    // not a pass. The UI thread's CPU time is no help here: a vCPU the hypervisor slows is
    // charged to the thread as CPU time (VMware reports no steal), as the control shows.
    (void)cpu_jank;
    const bool rest_ok = flat && realloc_ok && sizes > 0;
    // A software device's heap is its JIT's (lavapipe compiles shader variants into LLVM
    // buffers it keeps): no G11 verdict, the numbers stay in the detail.
    const char* verdict = w.gpu().software()                ? "software"
                          : rest_ok && !jank                ? "pass"
                          : rest_ok && jank && control.jank ? "jitter"
                                                            : "fail";
    rw.gate("G11", verdict,
            "%llu configures, %u sizes, %llu reallocations; frame p50 %.1f us, max %.1f us "
            "(%.2fx, %u over 2x); UI-thread CPU p50 %.1f us, max %.1f us (%.2fx, %u over 2x); "
            "run-queue delay %.1f ms, steal %llu ms; host control (a fixed %.0f us job, same "
            "cadence) max %.2fx its median, %u over 2x; heap %+lld B after the storm (%llu operator "
            "new, %llu malloc during it)",
            static_cast<unsigned long long>(configures), sizes,
            static_cast<unsigned long long>(reallocations), st.p50, st.max,
            st.p50 > 0 ? st.max / st.p50 : 0.0, over, sc.p50, sc.max,
            sc.p50 > 0 ? sc.max / sc.p50 : 0.0, cpu_over, run_delay_ms,
            static_cast<unsigned long long>(steal_ms), sc.p50,
            control.wall.p50 > 0 ? control.wall.max / control.wall.p50 : 0.0, control.over,
            heap.heap,
            static_cast<unsigned long long>(heap.news), static_cast<unsigned long long>(heap.mallocs));

    write_header(f, a, w, setup_of("S7").title, static_cast<unsigned>(log.frames.size()), load_before,
                 load_after);
    std::fputs("    \"stages\": {", f);
    bool first = true;
    write_frame_stages(f, log.frames, first);
    std::fputs("\n    },\n", f);
    std::fprintf(f,
                 "    \"counters\": {\"resizes\": %u, \"configures\": %llu, \"acks\": %llu, "
                 "\"sizes\": %u, \"reallocations\": %llu, \"frames\": %zu, \"frames_over_2x_median\": "
                 "%u, \"frames_cpu_over_2x_median\": %u, \"run_delay_ms\": %.2f, \"steal_ms\": %llu, "
                 "\"start\": [%d, %d], \"end\": [%d, %d]},\n",
                 a.storm, static_cast<unsigned long long>(configures),
                 static_cast<unsigned long long>(ws_b.acks - ws_a.acks), sizes,
                 static_cast<unsigned long long>(reallocations), log.frames.size(), over,
                 cpu_over, run_delay_ms, static_cast<unsigned long long>(steal_ms),
                 size_a.width, size_a.height, w.buffer_size().width, w.buffer_size().height);
    std::fprintf(f,
                 "    \"memory\": {\"storm_allocations\": %llu, \"storm_malloc_calls\": %llu, "
                 "\"heap_delta\": %lld, \"heap_flat_tolerance\": %lld, \"rss_anon_kb\": %ld},\n",
                 static_cast<unsigned long long>(heap.news),
                 static_cast<unsigned long long>(heap.mallocs), heap.heap, k_flat_bytes,
                 rss_anon_kb());
    std::fprintf(f,
                 "    \"client\": {\"control\": {\"work_us\": %.1f, \"p50\": %.1f, \"p95\": %.1f, "
                 "\"max\": %.1f, \"over_2x\": %u, \"jobs\": %u, \"gap_us\": 200000}},\n",
                 sc.p50, control.wall.p50, control.wall.p95, control.wall.max, control.over,
                 control.wall.samples);
    write_gates(f, rw);
    if (a.record_dir) {
        const std::string frames_path = std::string(a.record_dir) + "/s7-frames.json";
        write_frames_json(frames_path.c_str(), w, log.frames, {}, g.rect);
    }
    std::printf("  S7 [%s] %llu configures → %u sizes, %llu reallocations, %zu frames\n", a.tier,
                static_cast<unsigned long long>(configures), sizes,
                static_cast<unsigned long long>(reallocations), log.frames.size());
    print_frames(log.frames);
    print_gates(a, rw);
    return 0;
}

int run_s9(const run_args& a, lx::application& app, lx::app::window& w, std::FILE* f) {
    lx::ui::ui_node* button = find_key(w.root().tree(), k_s9_button);
    if (!button) {
        write_skip(a, "no button");
        return 1;
    }
    const lx::rect2i b = lx::ui::surface_bounds(*button);
    const int in_x = b.x + b.width / 2, in_y = b.y + b.height / 2;
    const int out_x = std::max(b.x - 80, 8), out_y = in_y;

    if (!a.socket) {
        // --connect: passive. The window is shown; real input comes from the kernel
        // (scripts/ui-bench.sh --e2e); the frames are written for the join.
        std::printf("lumen-ui-bench s9: ready app_id=%s button=%d,%d,%d,%d window=%d,%d in=%d,%d "
                    "out=%d,%d\n",
                    k_app_id, b.x, b.y, b.width, b.height, w.logical_size().width,
                    w.logical_size().height, in_x, in_y, out_x, out_y);
        std::fflush(stdout);
        frame_log log{};
        const u64 t0 = mono_ns();
        lx::runtime::event_loop& loop = app.event_loop();
        struct hctx {
            frame_log* log;
            const lx::app::window* w;
            u64 since;
        } hc{&log, &w, t0};
        lx::runtime::timer_source harvest{1'000'000'000, lx::runtime::task{[p = &hc] {
                                              p->log->harvest(*p->w, p->since, mono_ns() - 150'000'000ull);
                                          }}};
        loop.add_source(&harvest);
        run_for(app, static_cast<int>(a.s9_ms));
        loop.remove_source(&harvest);
        log.harvest(w, t0, 0);
        if (a.frames_out)
            write_frames_json(a.frames_out, w, log.frames, b, {});
        std::printf("lumen-ui-bench s9: %zu frames recorded\n", log.frames.size());
        return 0;
    }

    host_link h{};
    if (!h.connect(a.socket)) {
        write_skip(a, "no compositor command socket (S9 headless needs injected input)");
        return 0;
    }
    const geometry g = query_geometry(h);
    if (!g.ok || !g.mapped) {
        write_skip(a, "the window is not mapped");
        return 1;
    }
    char cmd[512];
    char reply[128];
    std::snprintf(cmd, sizeof cmd, "move %d %d", g.rect.x + out_x, g.rect.y + out_y);
    (void)h.call(cmd, reply, sizeof reply);
    run_for(app, 200);
    (void)settle(app, w);
    // Warm-up: a few crossings, so the first measured one does not pay a first draw.
    std::snprintf(cmd, sizeof cmd, "hover %d %d %d %d 40 8000 4 -", g.rect.x + in_x, g.rect.y + in_y,
                  g.rect.x + out_x, g.rect.y + out_y);
    h.send(cmd);
    (void)run_until_reply(app, w, h, reply, sizeof reply, 3000);
    run_for(app, 100);
    (void)settle(app, w);

    std::string inject_path;
    if (a.record_dir)
        inject_path = std::string(a.record_dir) + "/inject.jsonl";
    else
        inject_path = bench_socket_path(a.socket) + ".s9.jsonl";
    const unsigned count = std::max(8u, a.s9_ms / 8u); // 125 Hz
    frame_log log{};
    const double load_before = load1();
    std::vector<motion_rec> motions;
    motions.reserve(k_max_injections);
    heap_mark mark{};
    mark.take();
    const u64 t0 = mono_ns();
    std::snprintf(cmd, sizeof cmd, "hover %d %d %d %d %u 8000 4 %s", g.rect.x + in_x,
                  g.rect.y + in_y, g.rect.x + out_x, g.rect.y + out_y, count, inject_path.c_str());
    h.send(cmd);
    const bool done = run_until_reply(app, w, h, reply, sizeof reply,
                                      static_cast<int>(count * 8 + 5000), &log, t0);
    const heap_delta heap = since(mark);
    run_for(app, 200);
    (void)settle(app, w);
    log.harvest(w, t0, 0);
    const double load_after = load1();
    if (!done) {
        write_skip(a, "the compositor did not finish the hover motions");
        return 1;
    }
    read_motions(inject_path.c_str(), motions);
    if (!a.record_dir)
        ::unlink(inject_path.c_str());
    unsigned transitions = 0;
    const std::vector<latency_sample> samples = join_hover(motions, log.frames, transitions);
    std::vector<u64> to_present, to_commit, to_start;
    for (const latency_sample& s : samples) {
        to_present.push_back(s.to_present_ns);
        to_commit.push_back(s.to_commit_ns);
        to_start.push_back(s.to_start_ns);
    }
    const u64 refresh = refresh_ns_of(log.frames);
    const summary lat = summarize(to_present);
    const double period_us = static_cast<double>(refresh) / 1000.0;
    const double budget_us = period_us + 2000.0;
    result_writer rw{};
    const bool enough = samples.size() >= 20 && samples.size() * 10 >= transitions * 8;
    rw.gate("G6-proxy",
            w.gpu().software()         ? "software"
            : !enough                  ? "insufficient"
            : lat.p50 <= budget_us     ? "pass"
                                       : "fail",
            "headless proxy (virtual output, timer flips): input → presented median %.2f ms "
            "(%.2f periods), p95 %.2f ms (%.2f periods), %zu of %u transitions; budget 1 period "
            "+ 2 ms = %.2f ms",
            lat.p50 / 1000.0, lat.p50 / period_us, lat.p95 / 1000.0, lat.p95 / period_us,
            samples.size(), transitions, budget_us / 1000.0);
    rw.gate("G5", heap.news == 0 ? "pass" : "fail",
            "%llu operator new on the UI thread over %zu hover frames (%llu malloc)",
            static_cast<unsigned long long>(heap.news), log.frames.size(),
            static_cast<unsigned long long>(heap.mallocs));

    write_header(f, a, w, setup_of("S9").title, static_cast<unsigned>(log.frames.size()),
                 load_before, load_after);
    std::fputs("    \"stages\": {", f);
    bool first = true;
    write_frame_stages(f, log.frames, first);
    json_stage(f, "input_to_frame_start", summarize(to_start), first);
    json_stage(f, "input_to_commit", summarize(to_commit), first);
    json_stage(f, "input_to_present", lat, first);
    std::fputs("\n    },\n", f);
    std::fprintf(f,
                 "    \"counters\": {\"motions\": %zu, \"transitions\": %u, \"samples\": %zu, "
                 "\"frames\": %zu, \"refresh_ns\": %llu, \"seconds\": %.2f},\n",
                 motions.size(), transitions, samples.size(), log.frames.size(),
                 static_cast<unsigned long long>(refresh),
                 static_cast<double>(mono_ns() - t0) / 1e9);
    std::fprintf(f,
                 "    \"memory\": {\"allocations\": %llu, \"allocations_per_steady_frame\": %.6f, "
                 "\"malloc_calls\": %llu, \"mallinfo2_in_use_delta\": %lld, \"rss_anon_kb\": "
                 "%ld},\n",
                 static_cast<unsigned long long>(heap.news),
                 log.frames.empty() ? 0.0 : static_cast<double>(heap.news) / log.frames.size(),
                 static_cast<unsigned long long>(heap.mallocs), heap.heap, rss_anon_kb());
    std::fprintf(f,
                 "    \"client\": {\"latency_periods\": {\"p50\": %.3f, \"p95\": %.3f}, "
                 "\"g6_budget_us\": %.1f, \"proxy\": true, \"button\": [%d, %d, %d, %d]},\n",
                 lat.p50 / period_us, lat.p95 / period_us, budget_us, b.x, b.y, b.width, b.height);
    write_gates(f, rw);
    if (a.record_dir) {
        const std::string frames_path = std::string(a.record_dir) + "/client.json";
        write_frames_json(frames_path.c_str(), w, log.frames, b, g.rect);
    }
    std::printf("  S9 [%s] %zu motions, %u transitions, %zu samples; input→present p50 %.2f ms "
                "p95 %.2f ms (refresh %.2f ms)\n",
                a.tier, motions.size(), transitions, samples.size(), lat.p50 / 1000.0,
                lat.p95 / 1000.0, period_us / 1000.0);
    print_frames(log.frames);
    print_gates(a, rw);
    return 0;
}

[[nodiscard]] lx::gfx::client_tier tier_of(const char* t) { return lx::gfx::parse_client_tier(t); }

int client_run_main(const run_args& a) {
    die_with_parent();
    if (a.pin >= 0)
        (void)pin_to(a.pin);
    // The cap warnings are what a frame path logs; keep start-up's info lines out of the table.
    lx::trace::logger::global().set_min_level(lx::trace::level::warn);
    char arg0[] = "lumen-ui-bench";
    char* argv[] = {arg0, nullptr};
    lx::application app{1, argv};
    app.config().socket = a.socket;
    app.config().backend = tier_of(a.tier);
    app.config().allow_software_gpu = app.config().backend == lx::gfx::client_tier::vulkan;
    app.config().watch_theme = 0;
    if (auto started = app.start(); !started) {
        write_skip(a, lx::format_error(started.get_error()));
        return 1;
    }
    const window_setup ws = setup_of(a.scenario);
    lx::app::window_options o{};
    o.title = ws.title;
    o.app_id = k_app_id;
    o.size = ws.size;
    o.describe = ws.describe;
    lx::app::window w;
    if (auto opened = w.open(app, o); !opened) {
        write_skip(a, lx::format_error(opened.get_error()));
        return 1;
    }
    if (tier_of(a.tier) != lx::gfx::client_tier::none && w.tier() != tier_of(a.tier)) {
        char why[160];
        std::snprintf(why, sizeof why, "the %s tier is not available here (the window fell back to %s)",
                      a.tier, lx::gfx::client_tier_name(w.tier()));
        write_skip(a, why);
        return 0;
    }
    if (!app.run_until([&] { return w.timeline().first_presented_ns != 0 || !w.is_open(); }, 10'000) ||
        !w.is_open()) {
        write_skip(a, "the first frame was never presented");
        return 1;
    }
    (void)settle(app, w);
    std::FILE* f = a.result ? std::fopen(a.result, "w") : nullptr;
    std::FILE* out = f ? f : stdout;
    int code = 0;
    if (std::strcmp(a.scenario, "S1") == 0)
        code = run_s1(a, app, w, out);
    else if (std::strcmp(a.scenario, "S5") == 0)
        code = run_s5(a, app, w, out);
    else if (std::strcmp(a.scenario, "S7") == 0)
        code = run_s7(a, app, w, out);
    else if (std::strcmp(a.scenario, "S9") == 0)
        code = run_s9(a, app, w, out);
    if (f)
        std::fclose(f);
    w.close();
    return code;
}

// ════════════════════════════════════════════════════════════════════════════════════
// The orchestrator (`--client`)
// ════════════════════════════════════════════════════════════════════════════════════

struct client_options {
    bool scenarios[5]{true, true, true, true, true}; ///< S1 S5 S7 S9 S10
    bool tiers[4]{true, true, true, false};          ///< cpu gl vulkan auto ("all": the first three)
    const char* json = nullptr;
    const char* compositor = "host";
    bool connect = false;
    unsigned frames = 600;
    unsigned warmup = 120;
    unsigned idle_ms = 10'000;
    unsigned s10_runs = 10;
    unsigned s9_ms = 3'000;
    unsigned storm = 30;
    const char* gates = nullptr;
    const char* record_dir = nullptr;
    const char* frames_out = nullptr;
    int pin_client = -2;     ///< -2: the last CPU; -1: none
    int pin_compositor = -2; ///< -2: CPU 0 when there are two or more; -1: none
    const char* git = "";
    const char* preset = "release";
};

constexpr const char* k_scenario_ids[5] = {"S1", "S5", "S7", "S9", "S10"};
constexpr const char* k_tier_ids[4] = {"cpu", "gl", "vulkan", "auto"};

void client_usage() {
    std::fputs(
        "usage: lumen-ui-bench --client [--scenario S1,S5,S7,S9,S10|all] [--tier cpu,gl,vulkan,auto|all]\n"
        "                      [--json PATH] [--compositor host|binary] [--connect] [--frames N]\n"
        "                      [--warmup N] [--idle-ms MS] [--s10-runs N] [--s9-ms MS] [--storm N]\n"
        "                      [--short] [--gate G1,G5,...] [--record-e2e DIR] [--frames-out PATH]\n"
        "                      [--pin-client CPU|none] [--pin-compositor CPU|none] [--git SHA]\n"
        "                      [--preset NAME]\n"
        "  --compositor host    (default) a headless lx::compositor on a private socket, configured\n"
        "                       as `lumen-compositor SOCK --virtual-outputs 1280x800 --no-control`,\n"
        "                       with a command socket for injected input (S5, S7, S9)\n"
        "  --compositor binary  the lumen-compositor binary, headless: S1 and S10 only\n"
        "  --connect            WAYLAND_DISPLAY: S1, S10, and S9 passive (scripts/ui-bench.sh --e2e)\n"
        "  --short              a quick run: 120 frames, 30 warm-up, 3 s idle, 3 start-ups, 10 resizes\n"
        "  --gate LIST          exit 1 when one of these gates fails (G1, G5, G6-proxy, G7-cold,\n"
        "                       G7-warm, G11, damage)\n"
        "  --record-e2e DIR     S9: keep the compositor's log, the injector's records and the\n"
        "                       window's frames in DIR (the sample scripts/ui-bench.sh --e2e\n"
        "                       --dry-run parses)\n",
        stderr);
}

[[nodiscard]] bool parse_list(const char* s, const char* const* ids, unsigned n, bool* out) {
    if (std::strcmp(s, "all") == 0) {
        for (unsigned i = 0; i < n; ++i)
            out[i] = true;
        return true;
    }
    for (unsigned i = 0; i < n; ++i)
        out[i] = false;
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", s);
    for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ",")) {
        bool found = false;
        for (unsigned i = 0; i < n; ++i)
            if (std::strcmp(tok, ids[i]) == 0) {
                out[i] = true;
                found = true;
            }
        if (!found) {
            std::fprintf(stderr, "lumen-ui-bench --client: unknown %s\n", tok);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parse_client(int argc, char** argv, client_options& o) {
    bool short_run = false;
    bool frames_set = false, warmup_set = false, idle_set = false, runs_set = false, storm_set = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        const auto take = [&]() -> const char* {
            if (!v) {
                std::fprintf(stderr, "lumen-ui-bench --client: %s needs a value\n", a);
                return nullptr;
            }
            ++i;
            return v;
        };
        const auto cpu = [](const char* s) {
            return std::strcmp(s, "none") == 0 ? -1 : static_cast<int>(std::strtol(s, nullptr, 10));
        };
        if (std::strcmp(a, "--client") == 0) {
        } else if (std::strcmp(a, "--scenario") == 0) {
            const char* s = take();
            if (!s || !parse_list(s, k_scenario_ids, 5, o.scenarios))
                return false;
        } else if (std::strcmp(a, "--tier") == 0 || std::strcmp(a, "--backend") == 0) {
            const char* s = take();
            if (!s || !parse_list(s, k_tier_ids, 4, o.tiers))
                return false;
            if (std::strcmp(s, "all") == 0)
                o.tiers[3] = false;
        } else if (std::strcmp(a, "--json") == 0) {
            if (!(o.json = take())) return false;
        } else if (std::strcmp(a, "--compositor") == 0) {
            if (!(o.compositor = take())) return false;
            if (std::strcmp(o.compositor, "host") != 0 && std::strcmp(o.compositor, "binary") != 0) {
                std::fprintf(stderr, "lumen-ui-bench --client: --compositor host|binary\n");
                return false;
            }
        } else if (std::strcmp(a, "--connect") == 0) {
            o.connect = true;
            o.compositor = "connect";
        } else if (std::strcmp(a, "--frames") == 0) {
            const char* s = take();
            if (!s) return false;
            o.frames = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
            frames_set = true;
        } else if (std::strcmp(a, "--warmup") == 0) {
            const char* s = take();
            if (!s) return false;
            o.warmup = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
            warmup_set = true;
        } else if (std::strcmp(a, "--idle-ms") == 0) {
            const char* s = take();
            if (!s) return false;
            o.idle_ms = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
            idle_set = true;
        } else if (std::strcmp(a, "--s10-runs") == 0) {
            const char* s = take();
            if (!s) return false;
            o.s10_runs = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
            runs_set = true;
        } else if (std::strcmp(a, "--s9-ms") == 0) {
            const char* s = take();
            if (!s) return false;
            o.s9_ms = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
        } else if (std::strcmp(a, "--storm") == 0) {
            const char* s = take();
            if (!s) return false;
            o.storm = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
            storm_set = true;
        } else if (std::strcmp(a, "--short") == 0) {
            short_run = true;
        } else if (std::strcmp(a, "--gate") == 0) {
            if (!(o.gates = take())) return false;
        } else if (std::strcmp(a, "--record-e2e") == 0) {
            if (!(o.record_dir = take())) return false;
        } else if (std::strcmp(a, "--frames-out") == 0) {
            if (!(o.frames_out = take())) return false;
        } else if (std::strcmp(a, "--pin-client") == 0) {
            const char* s = take();
            if (!s) return false;
            o.pin_client = cpu(s);
        } else if (std::strcmp(a, "--pin-compositor") == 0) {
            const char* s = take();
            if (!s) return false;
            o.pin_compositor = cpu(s);
        } else if (std::strcmp(a, "--pin") == 0) {
            const char* s = take();
            if (!s) return false;
            o.pin_client = cpu(s);
            if (o.pin_client < 0)
                o.pin_compositor = -1;
        } else if (std::strcmp(a, "--git") == 0) {
            if (!(o.git = take())) return false;
        } else if (std::strcmp(a, "--preset") == 0) {
            if (!(o.preset = take())) return false;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            client_usage();
            return false;
        } else {
            std::fprintf(stderr, "lumen-ui-bench --client: unknown option %s\n", a);
            client_usage();
            return false;
        }
    }
    if (short_run) {
        if (!frames_set) o.frames = 120;
        if (!warmup_set) o.warmup = 30;
        if (!idle_set) o.idle_ms = 3000;
        if (!runs_set) o.s10_runs = 3;
        if (!storm_set) o.storm = 10;
    }
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (o.pin_client == -2)
        o.pin_client = n > 1 ? static_cast<int>(n - 1) : -1;
    if (o.pin_compositor == -2)
        o.pin_compositor = n > 1 ? 0 : -1;
    return true;
}

[[nodiscard]] std::string self_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return ".";
    buf[n] = '\0';
    std::string s = buf;
    const std::size_t slash = s.rfind('/');
    return slash == std::string::npos ? "." : s.substr(0, slash);
}

/// fork + execv with `env` added to the environment (each "NAME=value"), stdout/stderr to
/// `log_fd` when ≥ 0. Returns the pid.
[[nodiscard]] pid_t spawn(const std::vector<std::string>& args, const std::vector<std::string>& env,
                          int log_fd, int keep_fd = -1) {
    std::vector<char*> argv;
    for (const std::string& s : args)
        argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e) {
        bool replaced = false;
        for (const std::string& add : env) {
            const std::size_t eq = add.find('=');
            if (std::strncmp(*e, add.c_str(), eq + 1) == 0)
                replaced = true;
        }
        if (!replaced)
            envs.emplace_back(*e);
    }
    for (const std::string& add : env)
        envs.push_back(add);
    std::vector<char*> envp;
    for (std::string& s : envs)
        envp.push_back(s.data());
    envp.push_back(nullptr);
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (log_fd >= 0) {
            ::dup2(log_fd, 1);
            ::dup2(log_fd, 2);
        }
        if (keep_fd >= 0) {
            const int flags = ::fcntl(keep_fd, F_GETFD);
            ::fcntl(keep_fd, F_SETFD, flags & ~FD_CLOEXEC);
        }
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    return pid;
}

/// Waits for `pid` up to `timeout_ms`; kills it past that. The exit status, or -1.
int wait_for(pid_t pid, int timeout_ms) {
    const u64 deadline = mono_ns() + static_cast<u64>(timeout_ms) * 1'000'000ull;
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0)
            return -1;
        if (mono_ns() > deadline) {
            ::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            return -1;
        }
        ::usleep(5000);
    }
}

void stop_process(pid_t pid) {
    if (pid <= 0)
        return;
    ::kill(pid, SIGTERM);
    if (wait_for(pid, 3000) == -1)
        (void)::waitpid(pid, nullptr, WNOHANG);
}

[[nodiscard]] std::string read_file(const std::string& path) {
    std::string out;
    if (std::FILE* f = std::fopen(path.c_str(), "r")) {
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void remove_tree(const std::string& dir) {
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
                continue;
            const std::string p = dir + "/" + e->d_name;
            struct stat st{};
            if (::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                remove_tree(p);
            else
                ::unlink(p.c_str());
        }
        ::closedir(d);
    }
    ::rmdir(dir.c_str());
}

struct startup_run {
    bool ok = false;
    long long commit_us = -1;
    long long present_us = -1;
    long long configure_us = -1;
    long long connect_us = -1;
    char tier[16]{};
};

/// One `lumen-hello --exit-after-first-present`, exec stamped right before execve.
[[nodiscard]] startup_run start_hello(const std::string& hello, const char* socket, const char* tier,
                                      const std::string& cache, int pin) {
    startup_run out{};
    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0)
        return out;
    std::vector<std::string> args = {hello, "--exit-after-first-present"};
    if (std::strcmp(tier, "auto") != 0) {
        args.push_back("--backend");
        args.push_back(tier);
    }
    if (socket) {
        args.push_back("--socket");
        args.push_back(socket);
    }
    if (std::strcmp(tier, "vulkan") == 0)
        args.push_back("--allow-software-gpu");
    std::vector<char*> argv;
    for (const std::string& s : args)
        argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e)
        if (std::strncmp(*e, "XDG_CACHE_HOME=", 15) != 0 && std::strncmp(*e, "LUMEN_EXEC_NS=", 14) != 0)
            envs.emplace_back(*e);
    envs.push_back("XDG_CACHE_HOME=" + cache);
    envs.push_back("LUMEN_EXEC_NS=00000000000000000000");
    std::vector<char*> envp;
    for (std::string& s : envs)
        envp.push_back(s.data());
    envp.push_back(nullptr);
    char* exec_slot = envp[envp.size() - 2] + std::strlen("LUMEN_EXEC_NS=");
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(pipefd[1], 1);
        ::dup2(pipefd[1], 2); // its log lines too: shown only when the start fails
        if (pin >= 0)
            (void)pin_to(pin);
        // The clock right before execve: what §8.2 calls exec. Digits written in place
        // (async-signal-safe), zero-padded to the slot's width.
        u64 t = mono_ns();
        for (int i = 19; i >= 0; --i) {
            exec_slot[i] = static_cast<char>('0' + t % 10);
            t /= 10;
        }
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string text;
    char buf[1024];
    for (;;) {
        pollfd pf{pipefd[0], POLLIN, 0};
        if (::poll(&pf, 1, 15000) <= 0)
            break;
        const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
        if (n <= 0)
            break;
        text.append(buf, static_cast<std::size_t>(n));
    }
    ::close(pipefd[0]);
    const int code = wait_for(pid, 15000);
    const char* line = std::strstr(text.c_str(), "lumen-hello startup:");
    if (!line || code != 0) {
        std::printf("  lumen-hello (%s) failed (exit %d):\n%s\n", tier, code, text.c_str());
        if (!line)
            return out;
    }
    const auto field = [&](const char* name) -> long long {
        const char* at = std::strstr(line, name);
        return at ? std::strtoll(at + std::strlen(name), nullptr, 10) : -1;
    };
    out.connect_us = field("exec_to_connect_us=");
    out.configure_us = field("exec_to_configure_us=");
    out.commit_us = field("exec_to_commit_us=");
    out.present_us = field("exec_to_present_us=");
    if (const char* t = std::strstr(line, "tier=")) {
        std::sscanf(t + 5, "%15s", out.tier);
    }
    out.ok = code == 0 && out.present_us > 0 && std::strstr(line, "exec_src=env") != nullptr;
    return out;
}

[[nodiscard]] std::string run_s10(const client_options& o, const char* tier, const char* socket,
                                  const std::string& hello, const std::string& work) {
    const std::string cache = work + "/cache";
    ::mkdir(cache.c_str(), 0700);
    // fontconfig's own cache is the system's (warm, not Lumen's to measure): linked in.
    if (const char* home = std::getenv("HOME")) {
        const std::string fc = std::string(home) + "/.cache/fontconfig";
        const std::string link = cache + "/fontconfig";
        struct stat st{};
        if (::stat(fc.c_str(), &st) == 0 && ::lstat(link.c_str(), &st) != 0)
            (void)::symlink(fc.c_str(), link.c_str());
    }
    const std::string lumen_cache = cache + "/lumen";
    std::vector<u64> cold_commit, cold_present, warm_commit, warm_present;
    char got_tier[16] = "";
    const double load_before = load1();
    unsigned failures = 0;
    for (unsigned i = 0; i < o.s10_runs; ++i) {
        remove_tree(lumen_cache);
        const startup_run r = start_hello(hello, socket, tier, cache, o.pin_client);
        if (!r.ok) {
            ++failures;
            continue;
        }
        std::snprintf(got_tier, sizeof got_tier, "%s", r.tier);
        cold_commit.push_back(static_cast<u64>(r.commit_us) * 1000);
        cold_present.push_back(static_cast<u64>(r.present_us) * 1000);
    }
    // Warm: one start primes the cache (not counted), then the runs.
    remove_tree(lumen_cache);
    (void)start_hello(hello, socket, tier, cache, o.pin_client);
    for (unsigned i = 0; i < o.s10_runs; ++i) {
        const startup_run r = start_hello(hello, socket, tier, cache, o.pin_client);
        if (!r.ok) {
            ++failures;
            continue;
        }
        warm_commit.push_back(static_cast<u64>(r.commit_us) * 1000);
        warm_present.push_back(static_cast<u64>(r.present_us) * 1000);
    }
    // What a warm cache holds on this host: pipeline-cache-*.bin (Vulkan), gl-program-*.bin
    // (GL with program binaries), fonts-* (font matches).
    bool pipeline_cache = false, program_cache = false, font_cache = false;
    if (DIR* d = ::opendir(lumen_cache.c_str())) {
        while (dirent* e = ::readdir(d)) {
            pipeline_cache |= std::strncmp(e->d_name, "pipeline-cache-", 15) == 0;
            program_cache |= std::strncmp(e->d_name, "gl-program-", 11) == 0;
            font_cache |= std::strncmp(e->d_name, "fonts-", 6) == 0;
        }
        ::closedir(d);
    }
    remove_tree(cache);
    const double load_after = load1();
    const bool software = std::strcmp(tier, "vulkan") == 0; // lavapipe is the only Vulkan here
    const summary cc = summarize(cold_commit), cp = summarize(cold_present);
    const summary wc = summarize(warm_commit), wp = summarize(warm_present);
    const bool warm_gpu_cache = std::strcmp(tier, "vulkan") == 0   ? pipeline_cache
                                : std::strcmp(tier, "gl") == 0     ? program_cache
                                                                   : true;
    // G7: ≤ 150 ms cold, ≤ 30 ms warm (first presented; p95, as §8.1 judges).
    // A software device's start-up is a CPU's JIT, not a GPU driver's: no G7 verdict.
    const char* cold_v = software            ? "software"
                         : cp.samples < 3     ? "insufficient"
                         : cp.p95 <= 150'000.0 ? "pass"
                                               : "fail";
    const char* warm_v = software            ? "software"
                         : wp.samples < 3     ? "insufficient"
                         : !warm_gpu_cache    ? "n/a"
                         : wp.p95 <= 30'000.0 ? "pass"
                                              : "fail";
    std::string out;
    char buf[4096];
    const host_info host = read_host();
    std::snprintf(buf, sizeof buf,
                  "  {\n    \"host\": {\"kernel\": \"%s\", \"cpus\": %u, \"arch\": \"%s\", "
                  "\"hostname\": \"%s\"},\n    \"driver\": \"%s\",\n    \"git\": \"%s\",\n"
                  "    \"preset\": \"%s\",\n    \"mode\": \"client\",\n    \"compositor\": \"%s\",\n"
                  "    \"scenario\": \"S10\",\n    \"title\": \"lumen-hello cold start x%u, with and "
                  "without a warm cache\",\n    \"backend\": \"%s\",\n    \"frames\": %u,\n"
                  "    \"warmup\": 1,\n    \"load\": {\"before\": %.2f, \"after\": %.2f},\n",
                  host.kernel, host.cpus, host.machine, host.nodename,
                  software ? "vulkan-software" : tier, o.git, o.preset, o.compositor, o.s10_runs,
                  tier, o.s10_runs, load_before, load_after);
    out += buf;
    out += "    \"stages\": {";
    struct named {
        const char* name;
        const summary* s;
    } stages[4] = {{"exec_to_commit_cold", &cc},
                   {"exec_to_present_cold", &cp},
                   {"exec_to_commit_warm", &wc},
                   {"exec_to_present_warm", &wp}};
    for (unsigned i = 0; i < 4; ++i) {
        std::snprintf(buf, sizeof buf,
                      "%s\n      \"%s\": {\"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": "
                      "%.3f, \"samples\": %u}",
                      i ? "," : "", stages[i].name, stages[i].s->p50, stages[i].s->p95,
                      stages[i].s->p99, stages[i].s->max, stages[i].s->samples);
        out += buf;
    }
    std::snprintf(buf, sizeof buf,
                  "\n    },\n    \"counters\": {\"runs\": %u, \"failures\": %u, \"tier\": \"%s\"},\n"
                  "    \"memory\": {},\n    \"client\": {\"warm_cache\": {\"pipeline_cache\": %s, "
                  "\"gl_program_binaries\": %s, \"font_matches\": %s}},\n",
                  o.s10_runs, failures, got_tier, pipeline_cache ? "true" : "false",
                  program_cache ? "true" : "false", font_cache ? "true" : "false");
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "    \"gates\": {\"G7-cold\": \"%s\", \"G7-warm\": \"%s\"},\n"
                  "    \"gate_details\": {\"G7-cold\": \"exec → first presented p50 %.1f ms, p95 "
                  "%.1f ms (first commit p50 %.1f ms) over %u runs; budget 150 ms\", \"G7-warm\": "
                  "\"exec → first presented p50 %.1f ms, p95 %.1f ms (first commit p50 %.1f ms); "
                  "budget 30 ms; %s\"}\n  }\n",
                  cold_v, warm_v, cp.p50 / 1000.0, cp.p95 / 1000.0, cc.p50 / 1000.0, cp.samples,
                  wp.p50 / 1000.0, wp.p95 / 1000.0, wc.p50 / 1000.0,
                  std::strcmp(tier, "gl") == 0 && !program_cache
                      ? "no GL program binary cache on this driver (GL_NUM_PROGRAM_BINARY_FORMATS 0): "
                        "the warm runs reuse the font matches only"
                  : std::strcmp(tier, "vulkan") == 0
                      ? (pipeline_cache ? "a warm VkPipelineCache on a software device (parity only)"
                                        : "no VkPipelineCache file was written (lavapipe keeps no "
                                          "pipelines in its cache and writes a truncated header, "
                                          "which ui_vk refuses to store): no warm case exists")
                      : "the CPU tier has no pipeline: warm is the font matches");
    out += buf;
    std::printf("  S10 [%s] cold: commit p50 %.1f ms, presented p50 %.1f / p95 %.1f ms; warm: "
                "commit p50 %.1f ms, presented p50 %.1f / p95 %.1f ms (%u failures)\n"
                "  S10 [%s] G7-cold %s, G7-warm %s\n",
                tier, cc.p50 / 1000.0, cp.p50 / 1000.0, cp.p95 / 1000.0, wc.p50 / 1000.0,
                wp.p50 / 1000.0, wp.p95 / 1000.0, failures, tier, cold_v, warm_v);
    std::fflush(stdout);
    return out;
}

pid_t g_host_pid = 0;

void on_orchestrator_signal(int sig) {
    if (g_host_pid > 0)
        ::kill(g_host_pid, SIGTERM);
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

int orchestrate(const client_options& o) {
    const std::string dir = self_dir();
    const std::string self = dir + "/lumen-ui-bench";
    const std::string hello = dir + "/lumen-hello";
    char socket[64];
    std::snprintf(socket, sizeof socket, "lumen-bench-%d", static_cast<int>(::getpid()));
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        std::fprintf(stderr, "lumen-ui-bench --client: XDG_RUNTIME_DIR is not set\n");
        return 2;
    }
    const std::string work = std::string(runtime) + "/" + socket + ".work";
    ::mkdir(work.c_str(), 0700);
    if (o.record_dir)
        ::mkdir(o.record_dir, 0755);
    ::signal(SIGINT, on_orchestrator_signal);
    ::signal(SIGTERM, on_orchestrator_signal);

    const host_info host = read_host();
    std::printf("lumen-ui-bench --client  compositor=%s  frames=%u  warmup=%u  idle=%u ms  "
                "pin client=%d compositor=%d  load=%.2f\nhost: %s %s, %u cpus, %s\n",
                o.compositor, o.frames, o.warmup, o.idle_ms, o.pin_client, o.pin_compositor,
                load1(), host.kernel, host.machine, host.cpus, host.cpu);
    std::fflush(stdout);

    // The compositor.
    const char* client_socket = nullptr;
    pid_t host_pid = 0;
    std::string comp_log = o.record_dir ? std::string(o.record_dir) + "/compositor.log"
                                        : work + "/compositor.log";
    if (!o.connect) {
        client_socket = socket;
        const int log_fd = ::open(comp_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        std::vector<std::string> env = {"LUMEN_CONFIG=0"};
        if (o.record_dir)
            env.push_back("LUMEN_LOG=debug");
        if (std::strcmp(o.compositor, "host") == 0) {
            int ready[2];
            if (::pipe2(ready, O_CLOEXEC) != 0)
                return 2;
            char pin[16];
            std::snprintf(pin, sizeof pin, "%d", o.pin_compositor);
            host_pid = spawn({self, "--compositor-host", socket, std::to_string(ready[1]), pin}, env,
                             log_fd, ready[1]);
            ::close(ready[1]);
            pollfd pf{ready[0], POLLIN, 0};
            char c = 0;
            const bool up = ::poll(&pf, 1, 15000) > 0 && ::read(ready[0], &c, 1) == 1 && c == 'R';
            ::close(ready[0]);
            if (!up) {
                std::fprintf(stderr, "lumen-ui-bench --client: the compositor host did not start "
                                     "(log: %s)\n",
                             comp_log.c_str());
                stop_process(host_pid);
                return 2;
            }
        } else {
            env.push_back("VK_LOADER_DRIVERS_DISABLE=*");
            host_pid = spawn({dir + "/lumen-compositor", socket, "--virtual-outputs", "1280x800",
                              "--no-control", "--ping-timeout", "0"},
                             env, log_fd);
            const std::string sock_path = std::string(runtime) + "/" + socket;
            struct stat st{};
            for (int i = 0; i < 300 && ::stat(sock_path.c_str(), &st) != 0; ++i)
                ::usleep(10'000);
            ::usleep(100'000);
        }
        if (log_fd >= 0)
            ::close(log_fd);
        g_host_pid = host_pid;
    }

    std::vector<std::string> records;
    bool gate_failed = false;
    bool infra_failed = false;
    struct stat st{};
    const bool render_node = ::stat("/dev/dri/renderD128", &st) == 0;
    for (unsigned t = 0; t < 4; ++t) {
        if (!o.tiers[t])
            continue;
        const char* tier = k_tier_ids[t];
        if (t > 0 && !render_node && t == 1) {
            std::printf("  [gl] skipped — no render node\n");
            continue;
        }
        std::printf("\n== tier %s%s\n", tier,
                    t == 2 ? " (on a software Vulkan device the rows are parity only)" : "");
        std::fflush(stdout);
        for (unsigned s = 0; s < 5; ++s) {
            if (!o.scenarios[s])
                continue;
            const char* sid = k_scenario_ids[s];
            const bool needs_host = s == 1 || s == 2 || (s == 3 && !o.connect);
            if (needs_host && std::strcmp(o.compositor, "host") != 0) {
                std::printf("  %s [%s] skipped — needs injected input (--compositor host)\n", sid, tier);
                continue;
            }
            if (s == 4) {
                records.push_back(run_s10(o, tier, client_socket, hello, work));
            } else {
                const std::string result = work + "/" + sid + "-" + tier + ".json";
                ::unlink(result.c_str());
                std::vector<std::string> args = {self, "--client-run", sid, tier, "--result", result,
                                                 "--frames", std::to_string(o.frames), "--warmup",
                                                 std::to_string(o.warmup), "--idle-ms",
                                                 std::to_string(o.idle_ms), "--s9-ms",
                                                 std::to_string(o.s9_ms), "--storm",
                                                 std::to_string(o.storm), "--git", o.git, "--preset",
                                                 o.preset, "--compositor-kind", o.compositor,
                                                 "--host-pid", std::to_string(host_pid), "--pin",
                                                 std::to_string(o.pin_client)};
                if (client_socket) {
                    args.push_back("--socket");
                    args.push_back(client_socket);
                }
                if (o.record_dir) {
                    args.push_back("--record-e2e");
                    args.push_back(o.record_dir);
                }
                if (o.frames_out) {
                    args.push_back("--frames-out");
                    args.push_back(o.frames_out);
                }
                const pid_t pid = spawn(args, {}, -1);
                const int budget = static_cast<int>(60'000 + o.idle_ms + o.frames * 40 + o.storm * 400 +
                                                    o.s9_ms * 2);
                const int code = wait_for(pid, budget);
                std::string text = read_file(result);
                if (code != 0)
                    infra_failed = true;
                if (text.empty()) {
                    std::printf("  %s [%s] FAILED to report (exit %d)\n", sid, tier, code);
                    infra_failed = true;
                    continue;
                }
                records.push_back(text);
                ::unlink(result.c_str());
            }
            // The gates asked for, on rows that are not a software device's.
            const std::string& rec = records.back();
            const bool software = rec.find("\"vulkan-software\"") != std::string::npos ||
                                  rec.find("\"gl-software\"") != std::string::npos;
            if (o.gates && !software) {
                char list[128];
                std::snprintf(list, sizeof list, "%s", o.gates);
                for (char* g = std::strtok(list, ","); g; g = std::strtok(nullptr, ",")) {
                    const std::string needle = std::string("\"") + g + "\": \"fail\"";
                    if (rec.find(needle) != std::string::npos) {
                        std::printf("  GATE %s FAILED (%s %s)\n", g, sid, tier);
                        gate_failed = true;
                    }
                }
            }
        }
    }
    if (host_pid > 0) {
        stop_process(host_pid);
        g_host_pid = 0;
    }
    if (o.json) {
        if (std::FILE* f = std::fopen(o.json, "w")) {
            std::fputs("[\n", f);
            for (std::size_t i = 0; i < records.size(); ++i) {
                std::string r = records[i];
                while (!r.empty() && (r.back() == '\n' || r.back() == ' '))
                    r.pop_back();
                std::fputs(r.c_str(), f);
                std::fputs(i + 1 < records.size() ? ",\n" : "\n", f);
            }
            std::fputs("]\n", f);
            std::fclose(f);
        }
    }
    if (!o.record_dir)
        ::unlink(comp_log.c_str());
    remove_tree(work);
    if (gate_failed)
        return 1;
    return infra_failed ? 2 : 0;
}

} // namespace

bool bench_client_mode(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--client") == 0 || std::strcmp(argv[i], "--client-run") == 0 ||
            std::strcmp(argv[i], "--compositor-host") == 0)
            return true;
    return false;
}

int bench_client_main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--compositor-host") == 0)
        return host_main(argv[2], std::atoi(argv[3]), argc >= 5 ? std::atoi(argv[4]) : -1);
    if (argc >= 4 && std::strcmp(argv[1], "--client-run") == 0) {
        run_args a{};
        a.scenario = argv[2];
        a.tier = argv[3];
        for (int i = 4; i + 1 < argc; i += 2) {
            const char* k = argv[i];
            const char* v = argv[i + 1];
            if (std::strcmp(k, "--result") == 0) a.result = v;
            else if (std::strcmp(k, "--socket") == 0) a.socket = v;
            else if (std::strcmp(k, "--frames") == 0) a.frames = static_cast<unsigned>(std::atoi(v));
            else if (std::strcmp(k, "--warmup") == 0) a.warmup = static_cast<unsigned>(std::atoi(v));
            else if (std::strcmp(k, "--idle-ms") == 0) a.idle_ms = static_cast<unsigned>(std::atoi(v));
            else if (std::strcmp(k, "--s9-ms") == 0) a.s9_ms = static_cast<unsigned>(std::atoi(v));
            else if (std::strcmp(k, "--storm") == 0) a.storm = static_cast<unsigned>(std::atoi(v));
            else if (std::strcmp(k, "--git") == 0) a.git = v;
            else if (std::strcmp(k, "--preset") == 0) a.preset = v;
            else if (std::strcmp(k, "--compositor-kind") == 0) a.compositor = v;
            else if (std::strcmp(k, "--host-pid") == 0) a.host_pid = std::atoi(v);
            else if (std::strcmp(k, "--pin") == 0) a.pin = std::atoi(v);
            else if (std::strcmp(k, "--record-e2e") == 0) a.record_dir = v;
            else if (std::strcmp(k, "--frames-out") == 0) a.frames_out = v;
        }
        return client_run_main(a);
    }
    client_options o{};
    if (!parse_client(argc, argv, o))
        return 2;
    return orchestrate(o);
}
