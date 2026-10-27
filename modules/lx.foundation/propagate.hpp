// `LX_RETURN_IF_ERROR(expr)` and `LX_TRY_ASSIGN(var, expr)`: propagate a failed
// `lx::result`. Macros, so they live in a header — a named module exports no macros, which is
// why `import lx.foundation;` gives `result` and not these. Include after
// `import lx.foundation;` (and `import std;`).
#pragma once

/// Propagate `result<void>` or `result<T>` failure from the current function.
#define LX_RETURN_IF_ERROR(expr)                                                                   \
    do {                                                                                           \
        if (auto _lx_ret = (expr); !_lx_ret)                                                       \
            return _lx_ret.get_error();                                                            \
    } while (0)

/// Propagate `result<T>` failure; assign value to `var` on success.
#define LX_TRY_ASSIGN(var, expr)                                                                   \
    do {                                                                                           \
        auto _lx_try = (expr);                                                                     \
        if (!_lx_try)                                                                              \
            return _lx_try.get_error();                                                            \
        (var) = std::move(_lx_try).value();                                                        \
    } while (0)
