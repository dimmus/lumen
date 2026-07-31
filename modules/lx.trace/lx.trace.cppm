module;

#include <cstdio>
#include <cstdlib>

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

class scoped_span {
public:
    scoped_span(const char* category, const char* name);
    ~scoped_span();
    scoped_span(const scoped_span&) = delete;
    scoped_span& operator=(const scoped_span&) = delete;

private:
    const char* category_ = "";
    const char* name_ = "";
};

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

lx::trace::scoped_span::scoped_span(const char* category, const char* name)
    : category_{category ? category : ""}, name_{name ? name : ""} {
    if (should_log(level::trace, logger::global().min_level()))
        logger::global().log(level::trace, category_, name_);
}

lx::trace::scoped_span::~scoped_span() {
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
