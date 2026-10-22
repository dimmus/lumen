module;

#include <stdio.h>

// xdg-foreign's handle registry (design-app-compat-capture B.4, XF1): one table for v1 and
// v2 (wlroots' `wlr_xdg_foreign_registry`), so a window exported over either version can be
// imported over either — GTK 3 and older Firefox speak only v1, Qt 6 only v2, and a portal
// parents its dialog onto whatever the app exported. Wayland-free: an export is a toplevel
// id, its handle and an opaque owner (the wire's exported object).
//
// Handles are 32 hex digits from 128 random bits (getrandom): a sandboxed app that could
// guess another app's handle could parent its windows onto it. UI affinity. The bodies are
// in foreign_registry.cpp (the Clang 23 partition rule, see session_lock.cpp).

import lx.foundation;

export module lx.compositor:foreign_registry;

import std;

export namespace lx::compositor {

inline constexpr unsigned k_foreign_max_exports = 64;
/// 32 hex digits and the terminator.
inline constexpr unsigned k_foreign_handle_bytes = 33;

struct foreign_export {
    char handle[k_foreign_handle_bytes]{};
    lx::toplevel_id toplevel{};
    void* owner = nullptr;
    bool used = false;
};

class foreign_registry {
public:
    /// Registers an export of `toplevel` by `owner` and writes its fresh handle. 0 when the
    /// table is full or no random bytes could be read.
    [[nodiscard]] unsigned add(lx::toplevel_id toplevel, void* owner,
                               char (&handle)[k_foreign_handle_bytes]);
    void remove(unsigned id);
    /// The export a handle names (0: none — unknown, or withdrawn).
    [[nodiscard]] unsigned find(const char* handle) const;
    [[nodiscard]] const foreign_export* get(unsigned id) const;
    [[nodiscard]] unsigned count() const { return count_; }
    /// 128 random bits as 32 lowercase hex digits. False when the kernel gave none.
    static bool make_handle(char (&out)[k_foreign_handle_bytes]);

private:
    foreign_export exports_[k_foreign_max_exports]{};
    unsigned count_ = 0;
};

} // namespace lx::compositor
