module;

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

import lx.foundation;

export module lx.trace;

export namespace lx::trace {

enum class level { trace, debug, info, warn, error, critical };

using log_fn = void (*)(level lvl, const char* category, const char* message, void* user);

class logger {
public:
    static logger& global();

    /// Replace the log sink. Pass `nullptr` to restore the default stderr sink.
    void set_sink(log_fn fn, void* user = nullptr);
    void set_min_level(level min_level);
    [[nodiscard]] level min_level() const;

    void log(level lvl, const char* category, const char* message) const;
    void log_error(const lx::error& err, const char* category = "error") const;

private:
    log_fn sink_ = nullptr;
    void* sink_user_ = nullptr;
    level min_level_ = level::info;
    bool use_default_sink_ = true;
};

/// A named interval on the calling thread: a trace-level log line pair when trace logging
/// is on, and a slice in the timeline when one is recording. `category` and `name` must be
/// string literals (the timeline stores the pointers). Costs one relaxed load when neither
/// is on, so it can sit on the frame path.
class scoped_span {
public:
    scoped_span(const char* category, const char* name);
    ~scoped_span();
    scoped_span(const scoped_span&) = delete;
    scoped_span& operator=(const scoped_span&) = delete;

private:
    const char* category_ = "";
    const char* name_ = "";
    unsigned long long start_ns_ = 0;
};

// ── Timeline ──────────────────────────────────────────────────────────────────
//
// A Perfetto-readable record of what every thread did and when: spans, counters and
// instants, with thread names, written as Chrome trace-event JSON — ui.perfetto.dev and
// trace_processor both load it. Off unless `LUMEN_TRACE_FILE` names an output file when
// `timeline_start_from_env` runs; then events go into one preallocated buffer (lock-free
// append, no allocation) and are written by `timeline_flush`. Timestamps are
// CLOCK_MONOTONIC, the clock `perf record -k mono` and KMS use, so the timeline lines up
// with a CPU profile of the same run.

/// True while a timeline is recording.
[[nodiscard]] bool timeline_enabled() noexcept;
/// Starts recording when `LUMEN_TRACE_FILE` is set (`LUMEN_TRACE_EVENTS` caps the buffer,
/// default 2,000,000 events). Idempotent.
void timeline_start_from_env();
/// Names the calling thread for the OS (perf, top, gdb) and in the timeline.
void name_thread(const char* name);
/// A counter sample now. `name` must be a string literal.
void timeline_counter(const char* name, long long value);
/// An instant event now. `category` and `name` must be string literals.
void timeline_instant(const char* category, const char* name);
/// Stops recording and writes the timeline. Call once the threads that record have
/// stopped. False when nothing was recording or the file could not be written.
bool timeline_flush();

#define LX_TRACE_SCOPE(cat, name) \
    ::lx::trace::scoped_span _lx_span_##__LINE__{cat, name}

void trace_event(level lvl, const char* category, const char* name,
                 const char* key = nullptr, const char* value = nullptr);

/// Default sink: `[LEVEL] category: message` on stderr. Used until `set_sink` overrides.
void default_stderr_sink(level lvl, const char* category, const char* message, void* user);

} // namespace lx::trace

module :private;

namespace {

const char* level_name(lx::trace::level lvl) {
    switch (lvl) {
    case lx::trace::level::trace: return "TRACE";
    case lx::trace::level::debug: return "DEBUG";
    case lx::trace::level::info: return "INFO";
    case lx::trace::level::warn: return "WARN";
    case lx::trace::level::error: return "ERROR";
    case lx::trace::level::critical: return "CRITICAL";
    }
    return "?";
}

lx::trace::level level_from_env(const char* value) {
    if (!value || !value[0]) return lx::trace::level::info;
    switch (value[0]) {
    case 't':
    case 'T':
        return lx::trace::level::trace;
    case 'd':
    case 'D':
        return lx::trace::level::debug;
    case 'i':
    case 'I':
        return lx::trace::level::info;
    case 'w':
    case 'W':
        return lx::trace::level::warn;
    case 'e':
    case 'E':
        return lx::trace::level::error;
    case 'c':
    case 'C':
        return lx::trace::level::critical;
    default:
        return lx::trace::level::info;
    }
}

lx::trace::level read_env_min_level() {
    if (const char* env = std::getenv("LUMEN_LOG")) return level_from_env(env);
#if !defined(NDEBUG)
    return lx::trace::level::debug;
#else
    return lx::trace::level::info;
#endif
}

bool should_log(lx::trace::level msg, lx::trace::level min) {
    return static_cast<int>(msg) >= static_cast<int>(min);
}

} // namespace

void lx::trace::default_stderr_sink(level lvl, const char* category, const char* message,
                                    void* user) {
    (void)user;
    const char* cat = category ? category : "lumen";
    const char* msg = message ? message : "";
    std::fprintf(stderr, "[%s] %s: %s\n", level_name(lvl), cat, msg);
    std::fflush(stderr);
}

lx::trace::logger& lx::trace::logger::global() {
    static logger instance;
    static bool initialized = false;
    if (!initialized) {
        instance.min_level_ = read_env_min_level();
        instance.sink_ = default_stderr_sink;
        instance.use_default_sink_ = true;
        initialized = true;
    }
    return instance;
}

void lx::trace::logger::set_sink(log_fn fn, void* user) {
    if (fn) {
        sink_ = fn;
        sink_user_ = user;
        use_default_sink_ = false;
    } else {
        sink_ = default_stderr_sink;
        sink_user_ = nullptr;
        use_default_sink_ = true;
    }
}

void lx::trace::logger::set_min_level(level min_level) { min_level_ = min_level; }

lx::trace::level lx::trace::logger::min_level() const { return min_level_; }

void lx::trace::logger::log(level lvl, const char* category, const char* message) const {
    if (!should_log(lvl, min_level_)) return;
    if (sink_) sink_(lvl, category, message, sink_user_);
}

void lx::trace::logger::log_error(const lx::error& err, const char* category) const {
    log(level::error, category ? category : "error", lx::format_error(err));
}

namespace {

#if defined(__linux__)
struct tl_event {
    const char* category;
    const char* name;
    unsigned long long ts;
    unsigned long long dur;
    long long value;
    int tid;
    unsigned char kind; // 0 span, 1 counter, 2 instant
};

struct tl_thread_name {
    int tid = 0;
    char name[32]{};
};

struct tl_state {
    std::atomic<bool> on{false};
    std::atomic<bool> started{false};
    tl_event* events = nullptr;
    unsigned long long capacity = 0;
    std::atomic<unsigned long long> next{0};
    std::atomic<unsigned long long> dropped{0};
    char path[512]{};
    std::mutex names_mutex;
    tl_thread_name names[64]{};
    unsigned name_count = 0;
};

tl_state& tl() {
    static tl_state s;
    return s;
}

unsigned long long mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<unsigned long long>(ts.tv_nsec);
}

int current_tid() {
    thread_local int tid = static_cast<int>(::syscall(SYS_gettid));
    return tid;
}

void tl_push(unsigned char kind, const char* category, const char* name, unsigned long long ts,
             unsigned long long dur, long long value) {
    tl_state& s = tl();
    const unsigned long long i = s.next.fetch_add(1, std::memory_order_relaxed);
    if (i >= s.capacity) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.events[i] = tl_event{category, name, ts, dur, value, current_tid(), kind};
}

/// JSON string body: names are literals, but a quote or backslash must not break the file.
void put_escaped(std::FILE* f, const char* s) {
    for (; s && *s; ++s) {
        const char c = *s;
        if (c == '"' || c == '\\')
            std::fputc('\\', f);
        if (static_cast<unsigned char>(c) >= 0x20)
            std::fputc(c, f);
    }
}
#endif

} // namespace

bool lx::trace::timeline_enabled() noexcept {
#if defined(__linux__)
    return tl().on.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

void lx::trace::timeline_start_from_env() {
#if defined(__linux__)
    tl_state& s = tl();
    if (s.started.exchange(true))
        return;
    const char* path = std::getenv("LUMEN_TRACE_FILE");
    if (!path || !path[0])
        return;
    unsigned long long cap = 2'000'000ull;
    if (const char* n = std::getenv("LUMEN_TRACE_EVENTS"); n && n[0])
        cap = std::strtoull(n, nullptr, 10);
    if (cap == 0)
        return;
    // Reserved, not committed: only pages the run actually fills become resident.
    void* mem = ::mmap(nullptr, cap * sizeof(tl_event), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        logger::global().log(level::error, "trace", "timeline: buffer mapping failed");
        return;
    }
    s.events = static_cast<tl_event*>(mem);
    s.capacity = cap;
    std::snprintf(s.path, sizeof(s.path), "%s", path);
    s.on.store(true, std::memory_order_release);
    char line[640];
    std::snprintf(line, sizeof(line), "timeline recording to %s (up to %llu events)", s.path, cap);
    logger::global().log(level::info, "trace", line);
#endif
}

void lx::trace::name_thread(const char* name) {
#if defined(__linux__)
    if (!name)
        return;
    char os_name[16];
    std::snprintf(os_name, sizeof(os_name), "%s", name); // the kernel keeps 15 bytes
    (void)pthread_setname_np(pthread_self(), os_name);
    tl_state& s = tl();
    std::lock_guard lock{s.names_mutex};
    const int tid = current_tid();
    for (unsigned i = 0; i < s.name_count; ++i) {
        if (s.names[i].tid == tid) {
            std::snprintf(s.names[i].name, sizeof(s.names[i].name), "%s", name);
            return;
        }
    }
    if (s.name_count < 64) {
        s.names[s.name_count].tid = tid;
        std::snprintf(s.names[s.name_count].name, sizeof(s.names[s.name_count].name), "%s", name);
        ++s.name_count;
    }
#else
    (void)name;
#endif
}

void lx::trace::timeline_counter(const char* name, long long value) {
#if defined(__linux__)
    if (timeline_enabled())
        tl_push(1, "counter", name, mono_ns(), 0, value);
#else
    (void)name;
    (void)value;
#endif
}

void lx::trace::timeline_instant(const char* category, const char* name) {
#if defined(__linux__)
    if (timeline_enabled())
        tl_push(2, category, name, mono_ns(), 0, 0);
#else
    (void)category;
    (void)name;
#endif
}

bool lx::trace::timeline_flush() {
#if defined(__linux__)
    tl_state& s = tl();
    if (!s.on.exchange(false))
        return false;
    std::FILE* f = std::fopen(s.path, "w");
    if (!f) {
        logger::global().log(level::error, "trace", "timeline: cannot open the output file");
        return false;
    }
    const int pid = static_cast<int>(::getpid());
    unsigned long long n = s.next.load(std::memory_order_acquire);
    if (n > s.capacity)
        n = s.capacity;
    std::fputs("{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n", f);
    char comm[32] = "lumen";
    if (std::FILE* c = std::fopen("/proc/self/comm", "r")) {
        if (std::fgets(comm, sizeof(comm), c))
            comm[std::strcspn(comm, "\n")] = '\0';
        std::fclose(c);
    }
    std::fprintf(f, "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":%d,\"tid\":%d,"
                    "\"args\":{\"name\":\"", pid, pid);
    put_escaped(f, comm);
    std::fputs("\"}}", f);
    {
        std::lock_guard lock{s.names_mutex};
        for (unsigned i = 0; i < s.name_count; ++i) {
            std::fprintf(f, ",\n{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":%d,\"tid\":%d,"
                            "\"args\":{\"name\":\"", pid, s.names[i].tid);
            put_escaped(f, s.names[i].name);
            std::fputs("\"}}", f);
        }
    }
    for (unsigned long long i = 0; i < n; ++i) {
        const tl_event& e = s.events[i];
        const double ts = static_cast<double>(e.ts) / 1000.0;
        if (e.kind == 0) {
            std::fprintf(f, ",\n{\"ph\":\"X\",\"cat\":\"");
            put_escaped(f, e.category);
            std::fputs("\",\"name\":\"", f);
            put_escaped(f, e.name);
            std::fprintf(f, "\",\"ts\":%.3f,\"dur\":%.3f,\"pid\":%d,\"tid\":%d}", ts,
                         static_cast<double>(e.dur) / 1000.0, pid, e.tid);
        } else if (e.kind == 1) {
            std::fputs(",\n{\"ph\":\"C\",\"name\":\"", f);
            put_escaped(f, e.name);
            std::fprintf(f, "\",\"ts\":%.3f,\"pid\":%d,\"tid\":%d,\"args\":{\"value\":%lld}}",
                         ts, pid, e.tid, e.value);
        } else {
            std::fputs(",\n{\"ph\":\"i\",\"s\":\"t\",\"cat\":\"", f);
            put_escaped(f, e.category);
            std::fputs("\",\"name\":\"", f);
            put_escaped(f, e.name);
            std::fprintf(f, "\",\"ts\":%.3f,\"pid\":%d,\"tid\":%d}", ts, pid, e.tid);
        }
    }
    std::fputs("\n]}\n", f);
    const bool ok = std::fclose(f) == 0;
    char line[700];
    std::snprintf(line, sizeof(line), "timeline: %llu events written to %s (%llu dropped: buffer full)",
                  n, s.path, s.dropped.load(std::memory_order_relaxed));
    logger::global().log(ok ? level::info : level::error, "trace", line);
    ::munmap(s.events, s.capacity * sizeof(tl_event));
    s.events = nullptr;
    s.capacity = 0;
    return ok;
#else
    return false;
#endif
}

lx::trace::scoped_span::scoped_span(const char* category, const char* name)
    : category_{category ? category : ""}, name_{name ? name : ""} {
#if defined(__linux__)
    if (timeline_enabled())
        start_ns_ = mono_ns();
#endif
    if (should_log(level::trace, logger::global().min_level()))
        logger::global().log(level::trace, category_, name_);
}

lx::trace::scoped_span::~scoped_span() {
#if defined(__linux__)
    if (start_ns_ && timeline_enabled()) {
        const unsigned long long end = mono_ns();
        tl_push(0, category_, name_, start_ns_, end - start_ns_, 0);
    }
#endif
    if (should_log(level::trace, logger::global().min_level())) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "~%s", name_);
        logger::global().log(level::trace, category_, buf);
    }
}

void lx::trace::trace_event(level lvl, const char* category, const char* name,
                            const char* key, const char* value) {
    if (!should_log(lvl, logger::global().min_level())) return;
    if (key && value) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s %s=%s", name ? name : "", key, value);
        logger::global().log(lvl, category ? category : "trace", buf);
    } else {
        logger::global().log(lvl, category ? category : "trace", name ? name : "");
    }
}
