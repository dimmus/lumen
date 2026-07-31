module;

#include <cstdio>

export module lx.foundation:error;

export import :result;

export namespace lx {

/// Domain-specific error codes (extend per subsystem during P0).
enum class io_err : int {
    none = 0,
    not_found = 1,
    permission_denied = 2,
    broken_pipe = 3,
};

enum class wayland_err : int {
    none = 0,
    bind_failed = 1,
    dispatch_failed = 2,
    protocol_violation = 3,
    client_disconnected = 4,
};

enum class vulkan_err : int {
    none = 0,
    device_lost = 1,
    out_of_memory = 2,
    import_failed = 3,
};

enum class gl_err : int {
    none = 0,
    /// EGL display, context or a required extension is unavailable.
    no_context = 1,
    /// Shader compile or program link failed.
    program_failed = 2,
    /// GBM allocation, EGLImage creation or FBO attachment failed.
    target_failed = 3,
    /// A client buffer could not be sampled (dma-buf import or upload).
    import_failed = 4,
    /// `glGetError` reported a failure during a frame.
    draw_failed = 5,
};

enum class drm_err : int {
    none = 0,
    open_failed = 1,
    mode_invalid = 2,
    page_flip_failed = 3,
    sync_failed = 4,
};

enum class protocol_err : int {
    none = 0,
    invalid_object = 1,
    invalid_argument = 2,
    unsupported = 3,
};

[[nodiscard]] constexpr const char* domain_name(error_domain domain) noexcept {
    switch (domain) {
    case error_domain::none: return "none";
    case error_domain::io: return "io";
    case error_domain::wayland: return "wayland";
    case error_domain::vulkan: return "vulkan";
    case error_domain::gl: return "gl";
    case error_domain::drm: return "drm";
    case error_domain::protocol: return "protocol";
    case error_domain::invalid_argument: return "invalid_argument";
    case error_domain::not_implemented: return "not_implemented";
    }
    return "unknown";
}

[[nodiscard]] constexpr error make_error(error_domain domain, int code,
                                         const char* message) noexcept {
    return {domain, code, message ? message : ""};
}

[[nodiscard]] inline error not_implemented(const char* what) noexcept {
    return make_error(error_domain::not_implemented, 0, what);
}

/// Format `error` into a thread-local buffer (not re-entrant).
[[nodiscard]] const char* format_error(const error& err) noexcept;

/// Write formatted error into `buf` (NUL-terminated). Returns bytes written (excl. NUL).
[[nodiscard]] int format_error_into(const error& err, char* buf, int capacity) noexcept;

template<typename T>
[[nodiscard]] constexpr bool failed(const result<T>& r) noexcept {
    return !r.ok();
}

/// Prefer get_error() by value — do not bind a reference to a temporary.
template<typename T>
[[nodiscard]] constexpr error error_of(const result<T>& r) noexcept {
    return r.get_error();
}

} // namespace lx

/// Propagate `result<void>` or `result<T>` failure from the current function.
#define LX_RETURN_IF_ERROR(expr)                                                     \
    do {                                                                             \
        if (auto _lx_ret = (expr); !_lx_ret)                                         \
            return decltype(_lx_ret){_lx_ret.get_error()};                            \
    } while (0)

/// Propagate `result<T>` failure; assign value to `var` on success.
#define LX_TRY_ASSIGN(var, expr)                                                     \
    do {                                                                             \
        auto _lx_try = (expr);                                                       \
        if (!_lx_try) return _lx_try.get_error();                                     \
        (var) = std::move(_lx_try).value();                                          \
    } while (0)


const char* lx::format_error(const error& err) noexcept {
    thread_local char buffer[512]{};
    (void)format_error_into(err, buffer, static_cast<int>(sizeof(buffer)));
    return buffer;
}

int lx::format_error_into(const error& err, char* buf, int capacity) noexcept {
    if (!buf || capacity <= 0) return 0;
    const char* msg = err.message ? err.message : "";
    const int n = std::snprintf(buf, static_cast<std::size_t>(capacity), "%s:%d: %s",
                                domain_name(err.domain), err.code, msg);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    if (n >= capacity) return capacity - 1;
    return n;
}
