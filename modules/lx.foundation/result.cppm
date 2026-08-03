module;

#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

export module lx.foundation:result;

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
};

struct error {
    error_domain domain = error_domain::none;
    int code = 0;
    const char* message = "";
};

/// Fallible return. Uses union storage so T need not be default-constructible.
template<typename T>
class [[nodiscard]] result {
public:
    result(T value) noexcept(std::is_nothrow_move_constructible_v<T>) // NOLINT
        : ok_{true} {
        ::new (static_cast<void*>(&value_)) T(std::move(value));
    }

    result(error err) noexcept // NOLINT
        : ok_{false} {
        ::new (static_cast<void*>(&error_)) error(err);
    }

    result(const result& other)
        requires std::is_copy_constructible_v<T>
        : ok_{other.ok_} {
        if (ok_)
            ::new (static_cast<void*>(&value_)) T(other.value_);
        else
            ::new (static_cast<void*>(&error_)) error(other.error_);
    }

    result(result&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : ok_{other.ok_} {
        if (ok_)
            ::new (static_cast<void*>(&value_)) T(std::move(other.value_));
        else
            ::new (static_cast<void*>(&error_)) error(other.error_);
    }

    result& operator=(const result& other)
        requires std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>
    {
        if (this == &other)
            return *this;
        if (ok_ == other.ok_) {
            if (ok_)
                value_ = other.value_;
            else
                error_ = other.error_;
        } else {
            destroy();
            ok_ = other.ok_;
            if (ok_)
                ::new (static_cast<void*>(&value_)) T(other.value_);
            else
                ::new (static_cast<void*>(&error_)) error(other.error_);
        }
        return *this;
    }

    result& operator=(result&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this == &other)
            return *this;
        if (ok_ == other.ok_) {
            if (ok_)
                value_ = std::move(other.value_);
            else
                error_ = other.error_;
        } else {
            destroy();
            ok_ = other.ok_;
            if (ok_)
                ::new (static_cast<void*>(&value_)) T(std::move(other.value_));
            else
                ::new (static_cast<void*>(&error_)) error(other.error_);
        }
        return *this;
    }

    ~result() { destroy(); }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] T& value() & noexcept {
#if !defined(NDEBUG)
        if (!ok_)
            std::abort();
#endif
        return value_;
    }
    [[nodiscard]] const T& value() const& noexcept {
#if !defined(NDEBUG)
        if (!ok_)
            std::abort();
#endif
        return value_;
    }
    [[nodiscard]] T&& value() && noexcept {
#if !defined(NDEBUG)
        if (!ok_)
            std::abort();
#endif
        return std::move(value_);
    }

    [[nodiscard]] error get_error() const noexcept {
        return ok_ ? error{} : error_;
    }

private:
    void destroy() noexcept {
        if (ok_)
            value_.~T();
        else
            error_.~error();
    }

    union {
        T value_;
        error error_;
    };
    bool ok_ = false;
};

template<>
class [[nodiscard]] result<void> {
public:
    result() noexcept : ok_{true} {}
    result(error err) noexcept : error_{err}, ok_{false} {} // NOLINT

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] error get_error() const noexcept { return ok_ ? error{} : error_; }

private:
    error error_{};
    bool ok_ = false;
};

} // namespace lx
