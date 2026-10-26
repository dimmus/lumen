module;

// Icons by name, for the panel's task entries (design-app-compat-capture B.8): the XDG Icon
// Theme lookup (icon-theme-spec 0.13) in the user's theme — GTK's `gtk-icon-theme-name`
// from settings.ini, else Adwaita — then its Inherits chain, then hicolor, then the unthemed
// directories; PNG decoded with libpng (LUMEN_HAS_PNG), SVG rendered with librsvg
// (LUMEN_HAS_RSVG) straight at the size asked for. Results are premultiplied ARGB8888 at
// exactly the requested pixel size, cached by (name, size) — a panel repaint never touches
// the file system, and a name is resolved once per size. Resolving one reads no directory
// either: a theme's GTK `icon-theme.cache` is mapped where it is current, and otherwise each
// directory is listed once, on first use.
//
// Shell process only, its one thread. Allocates when a theme index is read and when an
// icon is loaded (rare, event driven), never per frame.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(LUMEN_HAS_PNG)
#include <png.h>
#endif
import lx.foundation;
import lx.trace;

export module lx.shell:icon_theme;

import std;
import :svg;

export namespace lx::shell {

/// Resamples a square premultiplied ARGB image to `dst_size`²: area averaging when
/// shrinking, bilinear when enlarging.
void resample_icon(const std::uint32_t* src, int src_size, std::uint32_t* dst, int dst_size);

class icon_theme {
public:
    icon_theme() = default;
    /// The theme to search first (null or "": the user's GTK setting, else Adwaita).
    void set_theme(const char* name);
    [[nodiscard]] const char* theme() const { return theme_.c_str(); }
    /// `name` (an icon name, or an absolute path to a PNG/SVG) at `px`² pixels; null when it
    /// cannot be found or decoded (cached too: a missing icon is not searched again).
    [[nodiscard]] const std::uint32_t* lookup(const char* name, int px);
    /// The file `name` resolves to at `px` ("" when none). For tests and logs.
    [[nodiscard]] std::string find_file(const char* name, int px);
    /// Decoders built in.
    [[nodiscard]] static bool can_png();
    [[nodiscard]] static bool can_svg();

private:
    enum class dir_type : unsigned char { fixed, scalable, threshold };
    struct theme_dir {
        std::string path; // relative to the theme
        int size = 0;
        int scale = 1;
        int min_size = 0;
        int max_size = 0;
        int threshold = 2;
        dir_type type = dir_type::threshold;
    };
    /// What one base directory of a theme holds, so a lookup reads memory, not the file
    /// system: GTK's `icon-theme.cache` mapped when it is at least as new as the directory
    /// (the check GTK makes; GTK and Qt read the same file), else each directory's file
    /// names, read once on first use.
    struct theme_root {
        std::string path;
        const unsigned char* cache = nullptr;
        std::size_t cache_size = 0;
        /// Per entry of `theme_index::dirs`: its index in the cache's directory list, -1 none.
        std::vector<int> cache_dir;
        /// Per entry of `theme_index::dirs`, without a cache: its file names, sorted.
        std::vector<std::vector<std::string>> listing;
        std::vector<bool> listed;

        theme_root() = default;
        theme_root(const theme_root&) = delete;
        theme_root& operator=(const theme_root&) = delete;
        ~theme_root();
        /// Whether `name` + `ext` is in the theme's directory `dir` (`rel`, its path) here.
        [[nodiscard]] bool has(std::size_t dir, const std::string& rel, std::string_view name,
                               std::string_view ext);
    };
    struct theme_index {
        std::string name;
        /// Base directories the theme exists in (…/icons/<name>).
        std::vector<std::unique_ptr<theme_root>> roots;
        /// Its directories that exist in at least one root.
        std::vector<theme_dir> dirs;
        std::vector<std::string> inherits;
    };
    struct cached {
        std::string name;
        int px = 0;
        std::vector<std::uint32_t> pixels; // empty: not found
        unsigned long long used = 0;
    };
    static constexpr unsigned k_cache = 96;

    void load();
    [[nodiscard]] theme_index* index_of(const std::string& name);
    [[nodiscard]] static std::string find_in(theme_index& t, const char* name, int px);
    [[nodiscard]] static bool decode(const std::string& path, int px, std::vector<std::uint32_t>& out);

    std::string theme_;
    bool loaded_ = false;
    std::vector<std::string> bases_;
    std::vector<std::unique_ptr<theme_index>> themes_;
    std::vector<theme_index*> chain_;
    std::vector<cached> cache_;
    unsigned long long clock_ = 0;
};

} // namespace lx::shell

namespace {

constexpr lx::trace::category log_icon{"shell.icon"};

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/// A symbolic icon's file (icon-naming-spec: "-symbolic"; GTK's pre-rendered ".symbolic.png").
bool is_symbolic_file(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        return false;
    const std::string_view stem(path.data(), dot);
    return stem.ends_with("-symbolic") || stem.ends_with(".symbolic");
}

/// Symbolic icons are monochrome shapes in a dark grey meant for light backgrounds; the
/// shell's surfaces (panel, menus, cards, switcher) are dark, so they are drawn in its text
/// colour instead, as GTK and Plasma recolour them to their foreground.
void recolour_symbolic(std::vector<std::uint32_t>& px) {
    constexpr unsigned r = 0xE6, g = 0xE6, b = 0xE6;
    for (auto& p : px) {
        const unsigned a = p >> 24;
        p = (a << 24) | ((r * a / 255u) << 16) | ((g * a / 255u) << 8) | (b * a / 255u);
    }
}

bool dir_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// GTK's icon-theme.cache (gtk/gtkiconcache.c, written by gtk-update-icon-cache). Big-endian;
// a header {u16 major = 1, u16 minor = 0, u32 hash, u32 directory list}; the directory list
// {u32 n, u32 name[n]}; the hash {u32 buckets, u32 icon[buckets]}, each icon {u32 next,
// u32 name, u32 images} and the images {u32 n, then n × {u16 directory, u16 flags, u32 data}}.
// Offsets are bounds-checked: the file is read as it is on disk.
constexpr std::uint32_t k_cache_none = 0xFFFFFFFFu;
constexpr std::uint16_t k_cache_svg = 2;
constexpr std::uint16_t k_cache_png = 4;

std::uint32_t cache_u32(const unsigned char* c, std::size_t n, std::uint32_t at) {
    if (at > n || n - at < 4)
        return k_cache_none;
    return (std::uint32_t{c[at]} << 24) | (std::uint32_t{c[at + 1]} << 16) |
           (std::uint32_t{c[at + 2]} << 8) | std::uint32_t{c[at + 3]};
}

std::uint16_t cache_u16(const unsigned char* c, std::size_t n, std::uint32_t at) {
    if (at > n || n - at < 2)
        return 0xFFFFu;
    return static_cast<std::uint16_t>((c[at] << 8) | c[at + 1]);
}

std::string_view cache_str(const unsigned char* c, std::size_t n, std::uint32_t at) {
    if (at >= n)
        return {};
    const void* end = std::memchr(c + at, 0, n - at);
    if (!end)
        return {};
    return {reinterpret_cast<const char*>(c + at),
            static_cast<std::size_t>(static_cast<const unsigned char*>(end) - (c + at))};
}

/// Maps `root`/icon-theme.cache when it is a version 1.0 cache no older than `root` itself.
void map_icon_cache(const std::string& root, const unsigned char*& data, std::size_t& size) {
    struct stat dir{};
    if (::stat(root.c_str(), &dir) != 0)
        return;
    const int fd = ::open((root + "/icon-theme.cache").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_mtime >= dir.st_mtime && st.st_size >= 12) {
        void* m = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            const auto* c = static_cast<const unsigned char*>(m);
            const auto n = static_cast<std::size_t>(st.st_size);
            if (cache_u16(c, n, 0) == 1 && cache_u16(c, n, 2) == 0) {
                data = c;
                size = n;
            } else
                ::munmap(m, n);
        }
    }
    ::close(fd);
}

/// `dir`'s index in the cache's directory list, -1 when the cache has no such directory.
int cache_dir_index(const unsigned char* c, std::size_t n, std::string_view dir) {
    const std::uint32_t list = cache_u32(c, n, 8);
    const std::uint32_t count = cache_u32(c, n, list);
    for (std::uint32_t i = 0; count != k_cache_none && i < count && i < 0xFFFFu; ++i) {
        const std::uint32_t at = cache_u32(c, n, list + 4 + 4 * i);
        if (at == k_cache_none)
            return -1;
        if (cache_str(c, n, at) == dir)
            return static_cast<int>(i);
    }
    return -1;
}

/// The offset of `name`'s image list in the cache, or k_cache_none.
std::uint32_t cache_images(const unsigned char* c, std::size_t n, std::string_view name) {
    if (name.empty())
        return k_cache_none;
    // gtkiconcache.c icon_name_hash, over signed chars.
    std::uint32_t h = static_cast<std::uint32_t>(static_cast<signed char>(name[0]));
    for (std::size_t i = 1; i < name.size(); ++i)
        h = (h << 5) - h + static_cast<std::uint32_t>(static_cast<signed char>(name[i]));
    const std::uint32_t hash = cache_u32(c, n, 4);
    const std::uint32_t buckets = cache_u32(c, n, hash);
    if (buckets == 0 || buckets == k_cache_none)
        return k_cache_none;
    std::uint32_t icon = cache_u32(c, n, hash + 4 + 4 * (h % buckets));
    for (int guard = 0; icon != k_cache_none && guard < 4096; ++guard) {
        if (cache_str(c, n, cache_u32(c, n, icon + 4)) == name)
            return cache_u32(c, n, icon + 8);
        icon = cache_u32(c, n, icon);
    }
    return k_cache_none;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= v.size()) {
        const std::size_t comma = v.find(',', at);
        std::string item = trim(v.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
        if (!item.empty())
            out.push_back(item);
        if (comma == std::string::npos)
            break;
        at = comma + 1;
    }
    return out;
}

/// `gtk-icon-theme-name` from GTK's settings.ini (4.0, then 3.0), or "".
std::string gtk_theme_setting() {
    std::string config;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && x[0])
        config = x;
    else if (const char* h = std::getenv("HOME"); h && h[0])
        config = std::string(h) + "/.config";
    for (const char* v : {"/gtk-4.0/settings.ini", "/gtk-3.0/settings.ini"}) {
        std::FILE* f = std::fopen((config + v).c_str(), "r");
        if (!f)
            continue;
        char line[512];
        std::string found;
        while (std::fgets(line, sizeof(line), f)) {
            std::string l = trim(line);
            if (l.rfind("gtk-icon-theme-name", 0) != 0)
                continue;
            const std::size_t eq = l.find('=');
            if (eq != std::string::npos)
                found = trim(l.substr(eq + 1));
        }
        std::fclose(f);
        if (!found.empty())
            return found;
    }
    return {};
}

} // namespace

void lx::shell::resample_icon(const std::uint32_t* src, int ss, std::uint32_t* dst, int ds) {
    if (!src || !dst || ss <= 0 || ds <= 0)
        return;
    if (ss == ds) {
        std::memcpy(dst, src, static_cast<std::size_t>(ss) * ss * 4u);
        return;
    }
    const auto ch = [](std::uint32_t p, int c) { return static_cast<double>((p >> (c * 8)) & 0xFFu); };
    if (ds < ss) {
        // Area average: each destination pixel covers `f` source pixels on each axis.
        const double f = static_cast<double>(ss) / ds;
        for (int y = 0; y < ds; ++y) {
            const double y0 = y * f, y1 = (y + 1) * f;
            for (int x = 0; x < ds; ++x) {
                const double x0 = x * f, x1 = (x + 1) * f;
                double acc[4]{};
                double area = 0;
                for (int sy = static_cast<int>(y0); sy < ss && sy < y1; ++sy) {
                    const double wy = std::fmin(y1, sy + 1.0) - std::fmax(y0, static_cast<double>(sy));
                    for (int sx = static_cast<int>(x0); sx < ss && sx < x1; ++sx) {
                        const double wx = std::fmin(x1, sx + 1.0) - std::fmax(x0, static_cast<double>(sx));
                        const double w = wx * wy;
                        const std::uint32_t p = src[static_cast<std::size_t>(sy) * ss + sx];
                        for (int c = 0; c < 4; ++c)
                            acc[c] += ch(p, c) * w;
                        area += w;
                    }
                }
                std::uint32_t out = 0;
                for (int c = 0; c < 4; ++c) {
                    const long v = area > 0 ? std::lround(acc[c] / area) : 0;
                    out |= static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v)) << (c * 8);
                }
                dst[static_cast<std::size_t>(y) * ds + x] = out;
            }
        }
        return;
    }
    // Bilinear, pixel centres aligned.
    const double f = static_cast<double>(ss) / ds;
    for (int y = 0; y < ds; ++y) {
        double sy = (y + 0.5) * f - 0.5;
        sy = sy < 0 ? 0 : (sy > ss - 1 ? ss - 1 : sy);
        const int y0 = static_cast<int>(sy);
        const int y1 = y0 + 1 < ss ? y0 + 1 : y0;
        const double ty = sy - y0;
        for (int x = 0; x < ds; ++x) {
            double sx = (x + 0.5) * f - 0.5;
            sx = sx < 0 ? 0 : (sx > ss - 1 ? ss - 1 : sx);
            const int x0 = static_cast<int>(sx);
            const int x1 = x0 + 1 < ss ? x0 + 1 : x0;
            const double tx = sx - x0;
            const std::uint32_t a = src[static_cast<std::size_t>(y0) * ss + x0];
            const std::uint32_t b = src[static_cast<std::size_t>(y0) * ss + x1];
            const std::uint32_t c2 = src[static_cast<std::size_t>(y1) * ss + x0];
            const std::uint32_t d = src[static_cast<std::size_t>(y1) * ss + x1];
            std::uint32_t out = 0;
            for (int c = 0; c < 4; ++c) {
                const double top = ch(a, c) * (1 - tx) + ch(b, c) * tx;
                const double bot = ch(c2, c) * (1 - tx) + ch(d, c) * tx;
                const long v = std::lround(top * (1 - ty) + bot * ty);
                out |= static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v)) << (c * 8);
            }
            dst[static_cast<std::size_t>(y) * ds + x] = out;
        }
    }
}

bool lx::shell::icon_theme::can_png() {
#if defined(LUMEN_HAS_PNG)
    return true;
#else
    return false;
#endif
}

bool lx::shell::icon_theme::can_svg() {
#if defined(LUMEN_HAS_RSVG)
    return true;
#else
    return false;
#endif
}

lx::shell::icon_theme::theme_root::~theme_root() {
    if (cache)
        ::munmap(const_cast<unsigned char*>(cache), cache_size);
}

bool lx::shell::icon_theme::theme_root::has(std::size_t dir, const std::string& rel,
                                            std::string_view name, std::string_view ext) {
    if (cache) {
        const int want = cache_dir[dir];
        const std::uint16_t flag = ext == ".png" ? k_cache_png : ext == ".svg" ? k_cache_svg : 0;
        const std::uint32_t images = cache_images(cache, cache_size, name);
        const std::uint32_t count = cache_u32(cache, cache_size, images);
        if (want < 0 || flag == 0 || count == k_cache_none)
            return false;
        for (std::uint32_t i = 0; i < count && i < 0xFFFFu; ++i) {
            const std::uint32_t at = images + 4 + 8 * i;
            if (cache_u16(cache, cache_size, at) == static_cast<std::uint32_t>(want))
                return (cache_u16(cache, cache_size, at + 2) & flag) != 0;
        }
        return false;
    }
    if (!listed[dir]) {
        listed[dir] = true;
        if (DIR* d = ::opendir((path + "/" + rel).c_str())) {
            while (const dirent* e = ::readdir(d))
                if (e->d_name[0] != '.' && e->d_type != DT_DIR)
                    listing[dir].emplace_back(e->d_name);
            ::closedir(d);
        }
        std::ranges::sort(listing[dir]);
    }
    std::string file;
    file.reserve(name.size() + ext.size());
    file.append(name).append(ext);
    return std::ranges::binary_search(listing[dir], file);
}

void lx::shell::icon_theme::set_theme(const char* name) {
    theme_ = name ? name : "";
    loaded_ = false;
    themes_.clear();
    chain_.clear();
    cache_.clear();
}

lx::shell::icon_theme::theme_index* lx::shell::icon_theme::index_of(const std::string& name) {
    for (const auto& t : themes_)
        if (t->name == name)
            return t->roots.empty() ? nullptr : t.get();
    auto t = std::make_unique<theme_index>();
    t->name = name;
    std::string index_path;
    for (const auto& b : bases_) {
        const std::string root = b + "/" + name;
        if (!dir_exists(root))
            continue;
        auto r = std::make_unique<theme_root>();
        r->path = root;
        map_icon_cache(root, r->cache, r->cache_size);
        t->roots.push_back(std::move(r));
        if (index_path.empty() && file_exists(root + "/index.theme"))
            index_path = root + "/index.theme";
    }
    if (!index_path.empty()) {
        std::FILE* f = std::fopen(index_path.c_str(), "r");
        std::vector<std::string> listed;
        std::vector<theme_dir> sections;
        theme_dir* cur = nullptr;
        bool in_theme = false;
        char raw[16384];
        while (f && std::fgets(raw, sizeof(raw), f)) {
            const std::string l = trim(raw);
            if (l.empty() || l[0] == '#')
                continue;
            if (l[0] == '[') {
                const std::string sec = l.substr(1, l.size() > 2 ? l.size() - 2 : 0);
                in_theme = sec == "Icon Theme";
                cur = nullptr;
                if (!in_theme) {
                    sections.push_back({});
                    cur = &sections.back();
                    cur->path = sec;
                }
                continue;
            }
            const std::size_t eq = l.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string k = trim(l.substr(0, eq));
            const std::string v = trim(l.substr(eq + 1));
            if (in_theme) {
                if (k == "Directories" || k == "ScaledDirectories")
                    for (auto& d : split_list(v))
                        listed.push_back(d);
                else if (k == "Inherits")
                    t->inherits = split_list(v);
            } else if (cur) {
                const int n = std::atoi(v.c_str());
                if (k == "Size")
                    cur->size = n;
                else if (k == "Scale")
                    cur->scale = n > 0 ? n : 1;
                else if (k == "MinSize")
                    cur->min_size = n;
                else if (k == "MaxSize")
                    cur->max_size = n;
                else if (k == "Threshold")
                    cur->threshold = n;
                else if (k == "Type")
                    cur->type = v == "Fixed"      ? dir_type::fixed
                                : v == "Scalable" ? dir_type::scalable
                                                  : dir_type::threshold;
            }
        }
        if (f)
            std::fclose(f);
        // Only listed directories, and only those that exist somewhere: hicolor lists ~400,
        // a system has a few dozen — the rest would be a stat each per lookup.
        for (auto& s : sections) {
            bool is_listed = false;
            for (const auto& d : listed)
                is_listed = is_listed || d == s.path;
            if (!is_listed || s.size <= 0)
                continue;
            bool present = false;
            for (const auto& r : t->roots)
                present = present || (r->cache ? cache_dir_index(r->cache, r->cache_size, s.path) >= 0
                                               : dir_exists(r->path + "/" + s.path));
            if (!present)
                continue;
            if (s.min_size <= 0)
                s.min_size = s.size;
            if (s.max_size <= 0)
                s.max_size = s.size;
            t->dirs.push_back(s);
        }
    }
    for (auto& r : t->roots) {
        if (r->cache)
            for (const auto& d : t->dirs)
                r->cache_dir.push_back(cache_dir_index(r->cache, r->cache_size, d.path));
        else {
            r->listing.resize(t->dirs.size());
            r->listed.assign(t->dirs.size(), false);
        }
    }
    themes_.push_back(std::move(t));
    return themes_.back()->roots.empty() ? nullptr : themes_.back().get();
}

void lx::shell::icon_theme::load() {
    if (loaded_)
        return;
    loaded_ = true;
    bases_.clear();
    const char* home = std::getenv("HOME");
    if (home && home[0])
        bases_.push_back(std::string(home) + "/.icons");
    if (const char* d = std::getenv("XDG_DATA_HOME"); d && d[0])
        bases_.push_back(std::string(d) + "/icons");
    else if (home && home[0])
        bases_.push_back(std::string(home) + "/.local/share/icons");
    const char* dirs = std::getenv("XDG_DATA_DIRS");
    {
        std::string all = dirs && dirs[0] ? dirs : "/usr/local/share:/usr/share";
        std::size_t at = 0;
        while (at <= all.size()) {
            const std::size_t colon = all.find(':', at);
            const std::string d = all.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
            if (!d.empty())
                bases_.push_back(d + "/icons");
            if (colon == std::string::npos)
                break;
            at = colon + 1;
        }
    }
    if (theme_.empty()) {
        if (const char* env = std::getenv("LUMEN_ICON_THEME"); env && env[0])
            theme_ = env;
        else
            theme_ = gtk_theme_setting();
        if (theme_.empty())
            theme_ = "Adwaita";
    }
    // The chain: the theme, its parents breadth-first (each once), hicolor last.
    std::vector<std::string> pending{theme_};
    for (std::size_t i = 0; i < pending.size() && i < 16; ++i) {
        if (pending[i] == "hicolor")
            continue;
        theme_index* t = index_of(pending[i]);
        if (!t)
            continue;
        chain_.push_back(t);
        for (const auto& p : t->inherits) {
            bool seen = false;
            for (const auto& q : pending)
                seen = seen || q == p;
            if (!seen)
                pending.push_back(p);
        }
    }
    if (theme_index* h = index_of("hicolor"))
        chain_.push_back(h);
    log_icon.info("icon theme {} ({} themes in the chain; png={} svg={})", theme_.c_str(),
                  static_cast<unsigned>(chain_.size()), can_png() ? 1 : 0, can_svg() ? 1 : 0);
}

std::string lx::shell::icon_theme::find_in(theme_index& t, const char* name, int px) {
    // icon-theme-spec LookupIcon, in pixels (Size × Scale): an exact match first, else the
    // directory with the smallest size distance.
    const char* exts[] = {can_png() ? ".png" : nullptr, can_svg() ? ".svg" : nullptr};
    std::string best;
    int best_distance = 1 << 30;
    for (std::size_t di = 0; di < t.dirs.size(); ++di) {
        const theme_dir& d = t.dirs[di];
        const int sc = d.scale;
        bool match = false;
        int distance = 0;
        switch (d.type) {
        case dir_type::fixed:
            match = d.size * sc == px;
            distance = std::abs(d.size * sc - px);
            break;
        case dir_type::scalable:
            match = d.min_size * sc <= px && px <= d.max_size * sc;
            distance = px < d.min_size * sc ? d.min_size * sc - px
                       : px > d.max_size * sc ? px - d.max_size * sc
                                              : 0;
            break;
        case dir_type::threshold:
            match = (d.size - d.threshold) * sc <= px && px <= (d.size + d.threshold) * sc;
            distance = px < (d.size - d.threshold) * sc   ? (d.size - d.threshold) * sc - px
                       : px > (d.size + d.threshold) * sc ? px - (d.size + d.threshold) * sc
                                                          : 0;
            break;
        }
        if (!match && distance >= best_distance)
            continue;
        for (auto& r : t.roots) {
            for (const char* ext : exts) {
                if (!ext || !r->has(di, d.path, name, ext))
                    continue;
                std::string path = r->path + "/" + d.path + "/" + name + ext;
                if (match)
                    return path;
                best = std::move(path);
                best_distance = distance;
            }
        }
    }
    return best;
}

std::string lx::shell::icon_theme::find_file(const char* name, int px) {
    if (!name || !name[0])
        return {};
    if (name[0] == '/')
        return file_exists(name) ? std::string(name) : std::string();
    load();
    for (theme_index* t : chain_)
        if (std::string f = find_in(*t, name, px); !f.empty())
            return f;
    // Unthemed: the base directories themselves, then /usr/share/pixmaps.
    std::vector<std::string> flat = bases_;
    flat.push_back("/usr/share/pixmaps");
    for (const auto& b : flat)
        for (const char* ext : {".png", ".svg"}) {
            if ((ext[1] == 'p' && !can_png()) || (ext[1] == 's' && !can_svg()))
                continue;
            std::string path = b + "/" + name + ext;
            if (file_exists(path))
                return path;
        }
    return {};
}

namespace {
/// Rasterised SVG icons, kept across sessions in `$XDG_CACHE_HOME/lumen/icons`: one file per
/// (source file, its modification time and size, pixel size), raw premultiplied ARGB words
/// behind a 16-byte header. The panel's icons come from SVG for most applications, and
/// rendering one needs librsvg, which is loaded only on a cache miss (`svg_document`).
struct icon_cache_header {
    char magic[4] = {'L', 'X', 'I', '1'};
    std::uint32_t px = 0;
    std::uint64_t key = 0;
};

std::string icon_cache_dir() {
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x == '/')
        return std::string{x} + "/lumen/icons";
    if (const char* h = std::getenv("HOME"); h && *h == '/')
        return std::string{h} + "/.cache/lumen/icons";
    return {};
}

/// FNV-1a over what makes a rasterisation stale: the path, its mtime and size, the pixels.
std::uint64_t icon_cache_key(const std::string& path, const struct stat& st, int px) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* data, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(path.data(), path.size());
    const long long mtime_ns =
        static_cast<long long>(st.st_mtim.tv_sec) * 1'000'000'000ll + st.st_mtim.tv_nsec;
    const long long size = static_cast<long long>(st.st_size);
    mix(&mtime_ns, sizeof mtime_ns);
    mix(&size, sizeof size);
    mix(&px, sizeof px);
    return h;
}

bool icon_cache_read(const std::string& file, std::uint64_t key, int px,
                     std::vector<std::uint32_t>& out) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    icon_cache_header h{};
    const std::size_t bytes = static_cast<std::size_t>(px) * px * 4u;
    bool ok = ::pread(fd, &h, sizeof h, 0) == static_cast<ssize_t>(sizeof h) &&
              std::memcmp(h.magic, "LXI1", 4) == 0 && h.px == static_cast<std::uint32_t>(px) &&
              h.key == key &&
              ::pread(fd, out.data(), bytes, sizeof h) == static_cast<ssize_t>(bytes);
    ::close(fd);
    return ok;
}

void icon_cache_write(const std::string& dir, const std::string& file, std::uint64_t key, int px,
                      const std::vector<std::uint32_t>& pixels) {
    // mkdir -p of the two levels under the cache root; failures leave the icon uncached.
    const std::size_t lumen = dir.rfind('/');
    if (lumen != std::string::npos)
        (void)::mkdir(dir.substr(0, lumen).c_str(), 0700);
    (void)::mkdir(dir.c_str(), 0700);
    const std::string tmp = file + ".tmp" + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    icon_cache_header h{};
    h.px = static_cast<std::uint32_t>(px);
    h.key = key;
    const std::size_t bytes = static_cast<std::size_t>(px) * px * 4u;
    const bool ok = ::write(fd, &h, sizeof h) == static_cast<ssize_t>(sizeof h) &&
                    ::write(fd, pixels.data(), bytes) == static_cast<ssize_t>(bytes);
    ::close(fd);
    // Atomic: a reader sees the old file, no file, or the whole new one.
    if (!ok || ::rename(tmp.c_str(), file.c_str()) != 0)
        (void)::unlink(tmp.c_str());
}
} // namespace

bool lx::shell::icon_theme::decode(const std::string& path, int px, std::vector<std::uint32_t>& out) {
    const bool svg = path.size() > 4 && path.compare(path.size() - 4, 4, ".svg") == 0;
    out.assign(static_cast<std::size_t>(px) * px, 0u);
    if (svg) {
        struct stat st{};
        const std::string dir = icon_cache_dir();
        const bool cacheable = !dir.empty() && ::stat(path.c_str(), &st) == 0;
        const std::uint64_t key = cacheable ? icon_cache_key(path, st, px) : 0;
        char name[32];
        std::snprintf(name, sizeof name, "/%016llx.argb", static_cast<unsigned long long>(key));
        const std::string file = dir + name;
        if (cacheable && icon_cache_read(file, key, px, out))
            return true;
        // Premultiplied, native-endian 0xAARRGGBB (cairo's ARGB32): what the panel draws.
        svg_document doc;
        const bool ok = doc.open(path.c_str()) &&
                        doc.render(reinterpret_cast<unsigned char*>(out.data()), px, px, px * 4,
                                   {0.f, 0.f, static_cast<float>(px), static_cast<float>(px)});
        if (ok && cacheable)
            icon_cache_write(dir, file, key, px, out);
        return ok;
    }
#if defined(LUMEN_HAS_PNG)
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path.c_str()))
        return false;
    img.format = PNG_FORMAT_BGRA; // bytes B,G,R,A: 0xAARRGGBB in a little-endian word
    if (img.width == 0 || img.height == 0 || img.width > 4096 || img.height > 4096) {
        png_image_free(&img);
        return false;
    }
    std::vector<std::uint32_t> full(static_cast<std::size_t>(img.width) * img.height);
    if (!png_image_finish_read(&img, nullptr, full.data(), 0, nullptr))
        return false;
    for (auto& p : full) {
        const unsigned a = p >> 24;
        const unsigned r = ((p >> 16) & 0xFFu) * a / 255u;
        const unsigned g = ((p >> 8) & 0xFFu) * a / 255u;
        const unsigned b = (p & 0xFFu) * a / 255u;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
    // A non-square PNG is centred in a square of its larger edge first.
    const int edge = static_cast<int>(img.width > img.height ? img.width : img.height);
    std::vector<std::uint32_t> square(static_cast<std::size_t>(edge) * edge, 0u);
    const int ox = (edge - static_cast<int>(img.width)) / 2;
    const int oy = (edge - static_cast<int>(img.height)) / 2;
    for (unsigned y = 0; y < img.height; ++y)
        std::memcpy(square.data() + static_cast<std::size_t>(y + oy) * edge + ox,
                    full.data() + static_cast<std::size_t>(y) * img.width, img.width * 4u);
    resample_icon(square.data(), edge, out.data(), px);
    return true;
#else
    return false;
#endif
}

const std::uint32_t* lx::shell::icon_theme::lookup(const char* name, int px) {
    if (!name || !name[0] || px <= 0 || px > 512)
        return nullptr;
    ++clock_;
    for (auto& c : cache_) {
        if (c.px == px && c.name == name) {
            c.used = clock_;
            return c.pixels.empty() ? nullptr : c.pixels.data();
        }
    }
    cached entry{};
    entry.name = name;
    entry.px = px;
    entry.used = clock_;
    const std::string file = find_file(name, px);
    if (!file.empty() && !decode(file, px, entry.pixels))
        entry.pixels.clear();
    else if (!entry.pixels.empty() && is_symbolic_file(file))
        recolour_symbolic(entry.pixels);
    if (log_icon.enabled(lx::trace::level::debug)) {
        log_icon.debug("icon {} {}px -> {}{}", name, px,
                       file.empty() ? "(not found)" : file.c_str(),
                       !file.empty() && entry.pixels.empty() ? " (undecodable)" : "");
    }
    if (cache_.size() >= k_cache) {
        auto* oldest = &cache_[0];
        for (auto& c : cache_)
            if (c.used < oldest->used)
                oldest = &c;
        *oldest = std::move(entry);
        return oldest->pixels.empty() ? nullptr : oldest->pixels.data();
    }
    cache_.push_back(std::move(entry));
    return cache_.back().pixels.empty() ? nullptr : cache_.back().pixels.data();
}
