module;

// Bodies of `:foreign_registry`. See foreign_registry.cppm.

#if defined(__linux__)
#include <sys/random.h>
#endif
#include <stdio.h>

module lx.compositor;

import std;

import lx.foundation;
import :foreign_registry;

namespace lx::compositor {

bool foreign_registry::make_handle(char (&out)[k_foreign_handle_bytes]) {
    unsigned char bytes[16]{};
#if defined(__linux__)
    std::size_t got = 0;
    while (got < sizeof(bytes)) {
        const ssize_t n = ::getrandom(bytes + got, sizeof(bytes) - got, 0);
        if (n <= 0)
            return false;
        got += static_cast<std::size_t>(n);
    }
#else
    return false;
#endif
    static constexpr char k_hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        out[2 * i] = k_hex[bytes[i] >> 4];
        out[2 * i + 1] = k_hex[bytes[i] & 0xf];
    }
    out[32] = '\0';
    return true;
}

unsigned foreign_registry::add(lx::toplevel_id toplevel, void* owner,
                               char (&handle)[k_foreign_handle_bytes]) {
    for (unsigned i = 0; i < k_foreign_max_exports; ++i) {
        foreign_export& e = exports_[i];
        if (e.used)
            continue;
        if (!make_handle(handle))
            return 0;
        e = {};
        std::memcpy(e.handle, handle, k_foreign_handle_bytes);
        e.toplevel = toplevel;
        e.owner = owner;
        e.used = true;
        ++count_;
        return i + 1;
    }
    return 0;
}

void foreign_registry::remove(unsigned id) {
    if (id == 0 || id > k_foreign_max_exports || !exports_[id - 1].used)
        return;
    exports_[id - 1] = {};
    --count_;
}

unsigned foreign_registry::find(const char* handle) const {
    if (!handle || std::strlen(handle) != k_foreign_handle_bytes - 1)
        return 0;
    for (unsigned i = 0; i < k_foreign_max_exports; ++i)
        if (exports_[i].used && std::memcmp(exports_[i].handle, handle, k_foreign_handle_bytes) == 0)
            return i + 1;
    return 0;
}

const foreign_export* foreign_registry::get(unsigned id) const {
    if (id == 0 || id > k_foreign_max_exports)
        return nullptr;
    return exports_[id - 1].used ? &exports_[id - 1] : nullptr;
}

} // namespace lx::compositor
