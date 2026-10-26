module;

#include <stdio.h>
#include <time.h>

#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <math.h>

import lx.foundation;

export module lx.trace;

import std;

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

    /// Formats a line with `std::format` syntax and logs it. `fmt` is checked against the
    /// arguments at compile time. Nothing is formatted below the minimum level; the line is
    /// built on the stack (truncated past `k_max_line` bytes), so it never allocates and is
    /// safe on the frame path wherever `log` is. Log-line conventions: a `bool` prints as 1/0
    /// (scripts grep `on=1`), and a null C string as "(null)".
    template <typename... Args>
    void logf(level lvl, const char* category, std::format_string<Args...> fmt,
              Args&&... args) const {
        if (lvl < min_level_)
            return;
        vlog(lvl, category, fmt.get(), std::make_format_args(lvalue(log_arg(args))...));
    }

    /// Type-erased half of `logf`: the formatting machinery is instantiated here once,
    /// not at every call site.
    void vlog(level lvl, const char* category, std::string_view fmt,
              std::format_args args) const noexcept;

    static constexpr std::size_t k_max_line = 4096;

private:
    template <typename T>
    static decltype(auto) log_arg(const T& v) {
        if constexpr (std::is_same_v<T, bool>)
            return static_cast<int>(v);
        else if constexpr (std::is_array_v<T>)
            return static_cast<const char*>(v);
        else if constexpr (std::is_same_v<std::decay_t<T>, const char*> ||
                           std::is_same_v<std::decay_t<T>, char*>)
            return static_cast<const char*>(v ? v : "(null)");
        else
            return v;
    }
    template <typename T>
    static T& lvalue(T&& v) {
        return static_cast<T&>(v); // a named T&& is an xvalue in a return (P2266)
    }

    log_fn sink_ = nullptr;
    void* sink_user_ = nullptr;
    level min_level_ = level::info;
    bool use_default_sink_ = true;
};

/// A log category: the one way code outside `lx.trace` logs. Declare it once per file or
/// partition, then log through the method named after the level:
///
///     constexpr lx::trace::category log_surface{"compositor.surface"};
///     log_surface.debug("attach id={} w={}", id.id(), w);
///     log_surface.error(err);
///     if (log_surface.enabled(lx::trace::level::debug)) { ... }
///
/// The name must be a string literal (the timeline keeps the pointer), which the
/// `consteval` constructor enforces. Formatting is `logger::logf`: `std::format` syntax
/// checked at compile time, nothing built below the minimum level, no allocation. Text
/// known only at run time goes through `"{}"`.
class category {
public:
    consteval category(const char* name) noexcept : name_{name} {}

    [[nodiscard]] constexpr const char* name() const noexcept { return name_; }
    [[nodiscard]] bool enabled(level lvl) const noexcept {
        return lvl >= logger::global().min_level();
    }

    /// For a level chosen at run time; otherwise use the method named after the level.
    template <typename... Args>
    void log(level lvl, std::format_string<Args...> fmt, Args&&... args) const {
        logger::global().logf(lvl, name_, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) const {
        log(level::trace, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        log(level::debug, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        log(level::info, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        log(level::warn, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        log(level::error, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void critical(std::format_string<Args...> fmt, Args&&... args) const {
        log(level::critical, fmt, std::forward<Args>(args)...);
    }
    /// A failed `result`, logged once at the boundary that handles it.
    void error(const lx::error& err) const { logger::global().log_error(err, name_); }

private:
    const char* name_;
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
/// Names the calling thread in the timeline, and for the OS (perf, top -H, gdb) unless it
/// is the main thread, whose OS name is the process name.
void name_thread(const char* name);
/// A counter sample now. `name` must be a string literal.
void timeline_counter(const char* name, long long value);
/// An instant event now. `category` and `name` must be string literals.
void timeline_instant(const char* category, const char* name);
/// Stops recording and writes the timeline. Call once the threads that record have
/// stopped. False when nothing was recording or the file could not be written.
bool timeline_flush();

// `LX_TRACE_SCOPE(cat, name)`, a `scoped_span` for the rest of the block, is a macro and so
// lives in `lx.trace/scope.hpp`: a named module exports no macros.

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
    // Set up in the static's initializer, which runs once even when two threads log first
    // at the same time.
    static logger instance = [] {
        logger l;
        l.min_level_ = read_env_min_level();
        l.sink_ = default_stderr_sink;
        l.use_default_sink_ = true;
        return l;
    }();
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

/// Output iterator over a fixed buffer that drops what does not fit.
struct bounded_out {
    struct span {
        char* pos;
        char* end;
    };
    using difference_type = std::ptrdiff_t;
    span* s;
    bounded_out& operator*() { return *this; }
    bounded_out& operator=(char c) {
        if (s->pos != s->end)
            *s->pos++ = c;
        return *this;
    }
    bounded_out& operator++() { return *this; }
    bounded_out operator++(int) { return *this; }
};

} // namespace

void lx::trace::logger::vlog(level lvl, const char* category, std::string_view fmt,
                             std::format_args args) const noexcept {
    if (!should_log(lvl, min_level_) || !sink_)
        return;
    char line[k_max_line];
    bounded_out::span out{line, line + sizeof(line) - 1};
    try {
        std::vformat_to(bounded_out{&out}, fmt, args);
    } catch (...) {
        // Only reachable through a runtime `fmt` (logf checks its format at compile time).
        out.pos = line;
        for (char c : fmt)
            bounded_out{& out} = c;
    }
    *out.pos = '\0';
    sink_(lvl, category, line, sink_user_);
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
    logger::global().logf(level::info, "trace", "timeline recording to {} (up to {} events)",
                          s.path, cap);
#endif
}

void lx::trace::name_thread(const char* name) {
#if defined(__linux__)
    if (!name)
        return;
    const int tid = current_tid();
    // Not the main thread: its name is the process name that ps, top and pkill show, and
    // renaming it turned "lumen-compositor" into "lx-ui". The timeline still labels it.
    if (tid != static_cast<int>(::getpid())) {
        char os_name[16];
        std::snprintf(os_name, sizeof(os_name), "%s", name); // the kernel keeps 15 bytes
        (void)pthread_setname_np(pthread_self(), os_name);
    }
    tl_state& s = tl();
    std::lock_guard lock{s.names_mutex};
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
    logger::global().logf(ok ? level::info : level::error, "trace",
                          "timeline: {} events written to {} ({} dropped: buffer full)", n, s.path,
                          s.dropped.load(std::memory_order_relaxed));
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
        logger::global().logf(level::trace, category_, "~{}", name_);
    }
}
