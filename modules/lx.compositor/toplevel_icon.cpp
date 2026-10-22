module;

// Bodies of `:toplevel_icon`. See toplevel_icon.cppm for the model.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

module lx.compositor;

import std;

import :toplevel_icon;

namespace lx::compositor {

namespace {

[[nodiscard]] int edge_distance(int size) {
    const int d = size - k_icon_preferred_edge;
    return d < 0 ? -d : d;
}

/// Ranks `a` before `b`: nearer the preferred edge, then larger, then earlier.
[[nodiscard]] bool ranks_before(const icon_candidate* in, unsigned a, unsigned b) {
    const int da = edge_distance(in[a].size);
    const int db = edge_distance(in[b].size);
    if (da != db)
        return da < db;
    if (in[a].size != in[b].size)
        return in[a].size > in[b].size;
    return a < b;
}

[[nodiscard]] unsigned long long image_bytes(int size) {
    return static_cast<unsigned long long>(size) * static_cast<unsigned long long>(size) * 4ull;
}

} // namespace

unsigned select_icon_images(const icon_candidate* in, unsigned n, unsigned long long budget_bytes,
                            unsigned* out) {
    if (!in || !out)
        return 0;
    // Insertion into a ranked list of at most k_icon_max_images: n is small (≤ 8 tracked
    // buffers per icon object), and nothing here allocates.
    unsigned ranked[k_icon_max_images]{};
    unsigned count = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (in[i].size <= 0 || in[i].size > k_icon_max_edge)
            continue;
        unsigned at = count;
        while (at > 0 && ranks_before(in, i, ranked[at - 1]))
            --at;
        if (at >= k_icon_max_images)
            continue;
        const unsigned last = count < k_icon_max_images ? count : k_icon_max_images - 1;
        for (unsigned j = last; j > at; --j)
            ranked[j] = ranked[j - 1];
        ranked[at] = i;
        if (count < k_icon_max_images)
            ++count;
    }
    unsigned long long total = 0;
    for (unsigned i = 0; i < count; ++i)
        total += image_bytes(in[ranked[i]].size);
    if (total <= budget_bytes) {
        for (unsigned i = 0; i < count; ++i)
            out[i] = ranked[i];
        return count;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (image_bytes(in[ranked[i]].size) <= budget_bytes) {
            out[0] = ranked[i];
            return 1;
        }
    }
    return 0;
}

toplevel_icon_table::~toplevel_icon_table() {
    for (auto& e : entries_) {
        if (!e.used)
            continue;
        for (unsigned i = 0; i < e.count; ++i)
            if (e.images[i].fd >= 0)
                ::close(e.images[i].fd);
    }
}

unsigned toplevel_icon_table::create(const char* name) {
    for (unsigned i = 0; i < k_icon_table_max; ++i) {
        icon_entry& e = entries_[i];
        if (e.used)
            continue;
        e = {};
        e.used = true;
        e.refs = 1;
        if (name)
            std::snprintf(e.name, sizeof(e.name), "%s", name);
        ++live_;
        return i + 1;
    }
    return 0;
}

bool toplevel_icon_table::add_image(unsigned id, const void* pixels, unsigned stride, int size,
                                    int scale, bool opaque) {
    if (id == 0 || id > k_icon_table_max || !pixels)
        return false;
    icon_entry& e = entries_[id - 1];
    if (!e.used || e.count >= k_icon_max_images || size <= 0 || size > k_icon_max_edge ||
        stride < static_cast<unsigned>(size) * 4u)
        return false;
    const unsigned long long bytes = image_bytes(size);
    if (bytes > budget_left())
        return false;
#if defined(__linux__)
    const int fd = ::memfd_create("lumen-toplevel-icon", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return false;
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return false;
    }
    void* map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    const auto* src = static_cast<const unsigned char*>(pixels);
    auto* dst = static_cast<unsigned char*>(map);
    const unsigned row = static_cast<unsigned>(size) * 4u;
    for (int y = 0; y < size; ++y) {
        std::memcpy(dst + static_cast<std::size_t>(y) * row,
                    src + static_cast<std::size_t>(y) * stride, row);
        if (opaque) {
            auto* px = reinterpret_cast<std::uint32_t*>(dst + static_cast<std::size_t>(y) * row);
            for (int x = 0; x < size; ++x)
                px[x] |= 0xff000000u;
        }
    }
    ::munmap(map, bytes);
    // Sealed: the shell maps it read-only and nothing (the compositor included) can change or
    // resize it under that mapping.
    (void)::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    e.images[e.count++] = {fd, size, scale > 0 ? scale : 1};
    bytes_ += bytes;
    return true;
#else
    (void)stride;
    (void)scale;
    (void)opaque;
    return false;
#endif
}

void toplevel_icon_table::ref(unsigned id) {
    if (id == 0 || id > k_icon_table_max)
        return;
    icon_entry& e = entries_[id - 1];
    if (e.used)
        ++e.refs;
}

void toplevel_icon_table::unref(unsigned id) {
    if (id == 0 || id > k_icon_table_max)
        return;
    icon_entry& e = entries_[id - 1];
    if (!e.used || e.refs == 0)
        return;
    if (--e.refs > 0)
        return;
    for (unsigned i = 0; i < e.count; ++i) {
        if (e.images[i].fd >= 0) {
#if defined(__linux__)
            ::close(e.images[i].fd);
#endif
            bytes_ -= image_bytes(e.images[i].size);
        }
    }
    e = {};
    --live_;
}

const icon_entry* toplevel_icon_table::get(unsigned id) const {
    if (id == 0 || id > k_icon_table_max)
        return nullptr;
    const icon_entry& e = entries_[id - 1];
    return e.used ? &e : nullptr;
}

} // namespace lx::compositor
