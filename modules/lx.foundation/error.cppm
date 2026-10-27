export module lx.foundation:error;

import std;

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
    /// Client side (lx.wayland.client): no compositor at the socket.
    connect_failed = 5,
    /// Client side: a global the caller needs (wl_compositor, wl_shm, xdg_wm_base, or the
    /// one an extension object is made from) is not advertised.
    missing_global = 6,
    /// Client side: the connection died (EPIPE, the compositor exited, a protocol error
    /// ended it); `display::protocol_error()` says which.
    connection_lost = 7,
    /// Client side: libwayland could not create a proxy (out of memory).
    object_failed = 8,
};

enum class vulkan_err : int {
    none = 0,
    device_lost = 1,
    out_of_memory = 2,
    import_failed = 3,
    /// An image's memory, a semaphore or a fence could not be exported (dma-buf, opaque fd,
    /// sync_file) — a client swapchain's handover to the compositor.
    export_failed = 4,
    /// The device lacks what the caller needs (an extension, a format or modifier, a
    /// hardware device where a software one was refused).
    unsupported = 5,
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
    /// The context or driver lacks what the caller needs (GLES 3.1, a software GL_RENDERER
    /// where hardware is required, no format the compositor offers).
    unsupported = 6,
    /// A buffer's dma-buf planes or a frame's native fence could not be exported.
    export_failed = 7,
};

enum class drm_err : int {
    none = 0,
    open_failed = 1,
    mode_invalid = 2,
    page_flip_failed = 3,
    sync_failed = 4,
    /// A connector probe (GETRESOURCES / GETCONNECTOR) failed.
    probe_failed = 5,
    /// No CRTC (or primary plane) is left for an output.
    no_crtc = 6,
    /// The kernel refused a TEST_ONLY of the configuration.
    test_rejected = 7,
    /// The hotplug monitor (udev netlink) could not be opened.
    monitor_failed = 8,
    /// A DRM timeline syncobj operation (import, query, eventfd, signal, transfer) failed.
    timeline_failed = 9,
    /// The driver offers no atomic modesetting (DRM_CLIENT_CAP_ATOMIC refused), which the
    /// KMS path requires (`lx::drm::no_atomic_hint` says what to do about it).
    no_atomic = 10,
};

enum class protocol_err : int {
    none = 0,
    invalid_object = 1,
    invalid_argument = 2,
    unsupported = 3,
};

enum class ui_err : int {
    none = 0,
    /// A compiled theme (.lxtheme) that is truncated, has the wrong magic, sizes, offsets or
    /// checksum.
    theme_format = 1,
    /// A compiled theme of another format version.
    theme_version = 2,
    /// A theme source (Zed theme JSON) that is not valid JSON or not a theme family.
    theme_source = 3,
    /// An icon that could not be rasterised (no librsvg, an unreadable SVG, a full atlas).
    icon_raster = 4,
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
    case error_domain::ui: return "ui";
    }
    return "unknown";
}

[[nodiscard]] constexpr error make_error(error_domain domain, int code,
                                         const char* message) noexcept {
    return {domain, code, message ? message : ""};
}

/// The domain a subsystem's error-code enum belongs to: `make_error(drm_err::open_failed, …)`
/// needs no `error_domain` and no cast.
template <typename E>
struct error_domain_of;
template <>
struct error_domain_of<io_err> : std::integral_constant<error_domain, error_domain::io> {};
template <>
struct error_domain_of<wayland_err> : std::integral_constant<error_domain, error_domain::wayland> {
};
template <>
struct error_domain_of<vulkan_err> : std::integral_constant<error_domain, error_domain::vulkan> {};
template <>
struct error_domain_of<gl_err> : std::integral_constant<error_domain, error_domain::gl> {};
template <>
struct error_domain_of<drm_err> : std::integral_constant<error_domain, error_domain::drm> {};
template <>
struct error_domain_of<protocol_err>
    : std::integral_constant<error_domain, error_domain::protocol> {};
template <>
struct error_domain_of<ui_err> : std::integral_constant<error_domain, error_domain::ui> {};

template <typename E>
concept domain_code = std::is_enum_v<E> && requires { error_domain_of<E>::value; };

template <domain_code E>
[[nodiscard]] constexpr error make_error(E code, const char* message) noexcept {
    return make_error(error_domain_of<E>::value, std::to_underlying(code), message);
}

[[nodiscard]] constexpr error not_implemented(const char* what) noexcept {
    return make_error(error_domain::not_implemented, 0, what);
}

[[nodiscard]] constexpr error invalid_argument(const char* what) noexcept {
    return make_error(error_domain::invalid_argument, 0, what);
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
