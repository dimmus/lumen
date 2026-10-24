// lumen-shell's icon lookup (lx.shell:icon_theme): resolving names through a theme's GTK
// icon-theme.cache when it is current, through its directories when there is none or it is
// stale, and surviving a cache that is not one.
#include "lumen_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.shell;

namespace {

const char* icon_ext() {
    return lx::shell::icon_theme::can_png() ? ".png" : lx::shell::icon_theme::can_svg() ? ".svg" : nullptr;
}

void write_file(const std::string& path, const void* data, std::size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    LUMEN_CHECK(f != nullptr);
    if (size)
        std::fwrite(data, 1, size, f);
    std::fclose(f);
}

/// A gtk-update-icon-cache file for the theme's two directories, holding `icons`: each name
/// with the directories (indices) it is in, with the suffix flag of `icon_ext()`.
std::vector<unsigned char> make_cache(const std::vector<std::pair<std::string, std::vector<unsigned>>>& icons) {
    std::vector<unsigned char> b;
    const auto u16 = [&](unsigned v) {
        b.push_back(static_cast<unsigned char>(v >> 8));
        b.push_back(static_cast<unsigned char>(v));
    };
    const auto u32 = [&](std::uint32_t v) {
        u16(v >> 16);
        u16(v & 0xFFFFu);
    };
    const auto set32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<unsigned char>(v >> (24 - 8 * i));
    };
    const auto str = [&](std::string_view s) {
        const auto at = static_cast<std::uint32_t>(b.size());
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
        while (b.size() % 4)
            b.push_back(0);
        return at;
    };
    const std::uint16_t flag = std::string_view(icon_ext()) == ".png" ? 4 : 2;
    constexpr std::uint32_t buckets = 5;

    u16(1), u16(0), u32(0), u32(0);
    // Directory list.
    set32(8, static_cast<std::uint32_t>(b.size()));
    const std::size_t dir_slots = b.size() + 4;
    u32(2), u32(0), u32(0);
    set32(dir_slots, str("16x16/apps"));
    set32(dir_slots + 4, str("48x48/apps"));
    // Hash: icons chained by bucket.
    set32(4, static_cast<std::uint32_t>(b.size()));
    const std::size_t bucket_slots = b.size() + 4;
    u32(buckets);
    for (std::uint32_t i = 0; i < buckets; ++i)
        u32(0xFFFFFFFFu);
    for (const auto& [name, dirs] : icons) {
        std::uint32_t h = static_cast<std::uint32_t>(static_cast<signed char>(name[0]));
        for (std::size_t i = 1; i < name.size(); ++i)
            h = (h << 5) - h + static_cast<std::uint32_t>(static_cast<signed char>(name[i]));
        const std::size_t slot = bucket_slots + 4 * (h % buckets);
        const std::uint32_t name_at = str(name);
        const std::uint32_t images = static_cast<std::uint32_t>(b.size());
        u32(static_cast<std::uint32_t>(dirs.size()));
        for (const unsigned dir : dirs)
            u16(dir), u16(flag), u32(0);
        const auto icon = static_cast<std::uint32_t>(b.size());
        const std::uint32_t next = (std::uint32_t{b[slot]} << 24) | (std::uint32_t{b[slot + 1]} << 16) |
                                   (std::uint32_t{b[slot + 2]} << 8) | b[slot + 3];
        u32(next), u32(name_at), u32(images);
        set32(slot, icon);
    }
    return b;
}

/// A theme "lxtest" in a fresh data directory: 16x16/apps and 48x48/apps, each holding
/// `alpha` and `beta`, plus `gamma` in 48x48/apps only.
struct test_theme {
    std::string data;
    std::string root;
    std::string ext = icon_ext();

    test_theme() {
        char tmpl[] = "/tmp/lumen-icon-XXXXXX";
        data = ::mkdtemp(tmpl);
        ::mkdir((data + "/icons").c_str(), 0755);
        root = data + "/icons/lxtest";
        ::mkdir(root.c_str(), 0755);
        const char index[] = "[Icon Theme]\nName=lxtest\nDirectories=16x16/apps,48x48/apps\n\n"
                             "[16x16/apps]\nSize=16\nType=Fixed\n\n[48x48/apps]\nSize=48\nType=Fixed\n";
        write_file(root + "/index.theme", index, sizeof(index) - 1);
        for (const char* d : {"/16x16", "/48x48"}) {
            ::mkdir((root + d).c_str(), 0755);
            ::mkdir((root + d + "/apps").c_str(), 0755);
        }
        for (const char* n : {"alpha", "beta"})
            for (const char* d : {"/16x16/apps/", "/48x48/apps/"})
                write_file(root + d + n + ext, "", 0);
        write_file(root + "/48x48/apps/gamma" + ext, "", 0);
        ::setenv("HOME", data.c_str(), 1);
        ::setenv("XDG_DATA_HOME", (data + "/none").c_str(), 1);
        ::setenv("XDG_DATA_DIRS", data.c_str(), 1);
    }
    ~test_theme() { std::filesystem::remove_all(data); }

    /// Writes the cache, its modification time `age` seconds after (or before) the root's.
    void cache(const std::vector<unsigned char>& bytes, int age) const {
        const std::string path = root + "/icon-theme.cache";
        write_file(path, bytes.data(), bytes.size());
        struct stat st{};
        ::stat(root.c_str(), &st);
        timespec times[2] = {{st.st_mtim.tv_sec + age, 0}, {st.st_mtim.tv_sec + age, 0}};
        ::utimensat(AT_FDCWD, path.c_str(), times, 0);
        // Writing the cache changed the root's own time; put it back.
        timespec root_times[2] = {st.st_atim, st.st_mtim};
        ::utimensat(AT_FDCWD, root.c_str(), root_times, 0);
    }

    [[nodiscard]] std::string find(const char* name, int px) const {
        lx::shell::icon_theme t;
        t.set_theme("lxtest");
        std::string f = t.find_file(name, px);
        return f.starts_with(root) ? f.substr(root.size()) : f;
    }
};

} // namespace

LUMEN_TEST(directories_are_searched_without_a_cache) {
    if (!icon_ext())
        return;
    test_theme t;
    LUMEN_CHECK(t.find("alpha", 16) == "/16x16/apps/alpha" + t.ext);
    LUMEN_CHECK(t.find("alpha", 48) == "/48x48/apps/alpha" + t.ext);
    // No exact size: the nearest directory.
    LUMEN_CHECK(t.find("beta", 40) == "/48x48/apps/beta" + t.ext);
    LUMEN_CHECK(t.find("gamma", 16) == "/48x48/apps/gamma" + t.ext);
    LUMEN_CHECK(t.find("delta", 16).empty());
}

LUMEN_TEST(a_current_cache_answers_lookups) {
    if (!icon_ext())
        return;
    test_theme t;
    // The cache has alpha in both directories, gamma in 16x16 only (where no file is) and no
    // beta: what it says is what is found, as it is for GTK.
    t.cache(make_cache({{"alpha", {0, 1}}, {"gamma", {0}}}), 10);
    LUMEN_CHECK(t.find("alpha", 16) == "/16x16/apps/alpha" + t.ext);
    LUMEN_CHECK(t.find("alpha", 48) == "/48x48/apps/alpha" + t.ext);
    LUMEN_CHECK(t.find("gamma", 48) == "/16x16/apps/gamma" + t.ext);
    LUMEN_CHECK(t.find("beta", 16).empty());
}

LUMEN_TEST(a_stale_cache_is_ignored) {
    if (!icon_ext())
        return;
    test_theme t;
    t.cache(make_cache({{"alpha", {0}}}), -10);
    LUMEN_CHECK(t.find("beta", 48) == "/48x48/apps/beta" + t.ext);
    LUMEN_CHECK(t.find("gamma", 16) == "/48x48/apps/gamma" + t.ext);
}

LUMEN_TEST(a_corrupt_cache_finds_nothing_and_survives) {
    if (!icon_ext())
        return;
    test_theme t;
    // Every offset past the end of the file.
    std::vector<unsigned char> bad = {0, 1, 0, 0, 0x7F, 0xFF, 0xFF, 0xF0, 0x7F, 0xFF, 0xFF, 0xF0, 0, 0, 0, 0};
    t.cache(bad, 10);
    LUMEN_CHECK(t.find("alpha", 16).empty());
    // Truncated mid-way: the directory list and hash cut off.
    std::vector<unsigned char> cut = make_cache({{"alpha", {0}}, {"beta", {1}}});
    cut.resize(cut.size() / 2);
    t.cache(cut, 10);
    (void)t.find("alpha", 16);
    (void)t.find("beta", 48);
    // Not version 1.0: not a cache, so the directories are searched.
    std::vector<unsigned char> v2 = make_cache({{"alpha", {0}}});
    v2[1] = 2;
    t.cache(v2, 10);
    LUMEN_CHECK(t.find("beta", 48) == "/48x48/apps/beta" + t.ext);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
