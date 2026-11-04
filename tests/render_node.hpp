#pragma once

// The render node the GPU tests and benches open: the GPU that drives the display, not
// whichever node enumerated first. A hardcoded /dev/dri/renderD128 is the single-node dev
// VM's assumption; on a hybrid notebook that path is the discrete card, and the GL tests
// end up on a driver the compositor itself would never composite with (the panel hangs off
// the other card). The compositor opens the card with connectors; the tests want its node.
//
// Selection, first that opens: LUMEN_TEST_RENDER_NODE when set ("none" forces no GPU);
// the render node of the first card with a connected connector; then renderD nodes in
// index order. Resolved once per process and shared by every caller.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lumen_test {

namespace detail_render_node {

inline std::string read_file(const std::string& path) {
    std::string out;
    if (std::FILE* f = std::fopen(path.c_str(), "r")) {
        char buf[64];
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        out = buf;
    }
    return out;
}

/// The name ("card2") of the first DRM card with a connected connector, or "".
inline std::string card_with_connector() {
    if (DIR* d = ::opendir("/sys/class/drm")) {
        while (const dirent* e = ::readdir(d)) {
            const char* name = e->d_name;
            if (std::strncmp(name, "card", 4) != 0 || std::strchr(name, '-') == nullptr)
                continue; // cardN-CONNECTOR entries only
            const std::string status =
                read_file(std::string("/sys/class/drm/") + name + "/status");
            if (status.compare(0, 9, "connected") == 0) {
                ::closedir(d);
                return std::string(name, static_cast<size_t>(std::strchr(name, '-') - name));
            }
        }
        ::closedir(d);
    }
    return "";
}

/// The /dev/dri path of `card`'s render node via by-path (same PCI slot), or "".
inline std::string render_node_of_card(const std::string& card) {
    // Two passes: readdir order is arbitrary, so the render entry may come before the
    // card entry that names its slot.
    std::string found;
    if (DIR* d = ::opendir("/dev/dri/by-path")) {
        const auto target_of = [](const std::string& entry, char* buf) -> std::string {
            const ssize_t n = ::readlink((std::string("/dev/dri/by-path/") + entry).c_str(), buf,
                                         255);
            if (n <= 0)
                return {};
            buf[n] = '\0';
            const char* base = std::strrchr(buf, '/');
            return base ? base + 1 : buf;
        };
        char buf[256];
        std::string slot;
        while (const dirent* e = ::readdir(d)) {
            const std::string entry = e->d_name;
            if (entry.size() <= 5 || entry.compare(entry.size() - 5, 5, "-card") != 0)
                continue;
            if (target_of(entry, buf) == card) {
                slot = entry.substr(0, entry.size() - 5);
                break;
            }
        }
        if (!slot.empty()) {
            ::rewinddir(d);
            while (const dirent* e = ::readdir(d)) {
                const std::string entry = e->d_name;
                if (entry.size() <= 7 || entry.compare(entry.size() - 7, 7, "-render") != 0)
                    continue;
                if (entry.compare(0, slot.size(), slot) == 0) {
                    found = std::string("/dev/dri/") + target_of(entry, buf);
                    break;
                }
            }
        }
        ::closedir(d);
    }
    return found;
}

/// Every candidate path in selection order.
inline std::vector<std::string> candidates() {
    std::vector<std::string> out;
    if (const char* env = ::getenv("LUMEN_TEST_RENDER_NODE")) {
        if (std::strcmp(env, "none") != 0)
            out.emplace_back(env);
    } else {
        const std::string node = render_node_of_card(card_with_connector());
        if (!node.empty())
            out.push_back(node);
    }
    for (int i = 128; i < 256; ++i)
        out.push_back("/dev/dri/renderD" + std::to_string(i));
    return out;
}

} // namespace detail_render_node

/// The chosen render node path, "" when there is none ("none" counts as chosen-and-empty).
[[nodiscard]] inline const std::string& render_node_path() {
    static const std::string path = [] {
        if (const char* env = ::getenv("LUMEN_TEST_RENDER_NODE"))
            return std::strcmp(env, "none") == 0 ? std::string()
                                                 : std::string(env);
        for (const std::string& c : detail_render_node::candidates()) {
            struct stat st{};
            if (::stat(c.c_str(), &st) == 0)
                return c;
        }
        return std::string();
    }();
    return path;
}

/// Opens the test render node once (O_RDWR | O_CLOEXEC); -1 when there is none.
inline int open_render_node() {
    static const int fd = [] {
        if (render_node_path().empty())
            return -1;
        return ::open(render_node_path().c_str(), O_RDWR | O_CLOEXEC);
    }();
    return fd;
}

} // namespace lumen_test
