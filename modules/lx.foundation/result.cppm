export module lx.foundation:result;

import std;

export namespace lx {

enum class error_domain {
    none,
    io,
    wayland,
    vulkan,
    /// EGL / OpenGL ES — the hardware GL present path. Separate from `vulkan` so a log
    /// line says which renderer actually failed.
    gl,
    drm,
    protocol,
    invalid_argument,
    not_implemented,
    /// lx.ui: compiled themes, icon rasterisation.
    ui,
};

struct error {
    error_domain domain = error_domain::none;
    int code = 0;
    const char* message = "";
};

/// Fallible return: `std::expected<T, error>` (so `and_then`, `transform`, `value_or`, `*r`
/// all work) that also converts implicitly from an `error`, which keeps
/// `return make_error(...);` and `return other.get_error();` working in any function
/// returning a `result`. `value()` on a failed result throws `std::bad_expected_access`.
/// `and_then` callables return `std::expected<U, lx::error>` (the standard requires an
/// `expected` specialization); a `result<U>` is built from that.
///
/// Inside this class `error` names `std::expected::error()`; the type is `lx::error`.
template <typename T>
class [[nodiscard]] result : public std::expected<T, lx::error> {
    using base = std::expected<T, lx::error>;

public:
    using base::base;
    result(lx::error err) noexcept : base(std::unexpect, err) {} // NOLINT
    /// What the monadic operations return.
    result(base b) noexcept(std::is_nothrow_move_constructible_v<base>) // NOLINT
        : base(std::move(b)) {}

    [[nodiscard]] bool ok() const noexcept { return this->has_value(); }
    [[nodiscard]] lx::error get_error() const noexcept {
        return this->has_value() ? lx::error{} : this->error();
    }
};

} // namespace lx
