module;

// Icons by name, for the panel's task entries (design-app-compat-capture B.8): the XDG Icon
// Theme lookup (icon-theme-spec 0.13) in the user's theme — GTK's `gtk-icon-theme-name`
// from settings.ini, else Adwaita — then its Inherits chain, then hicolor, then the unthemed
// directories; PNG decoded with libpng (LUMEN_HAS_PNG), SVG rendered with librsvg
// (LUMEN_HAS_RSVG) straight at the size asked for. Results are premultiplied ARGB8888 at
// exactly the requested pixel size, cached by (name, size) — a panel repaint never touches
// the file system, and a name is resolved once per size.
//
// Shell process only, its one thread. Allocates when a theme index is read and when an
// icon is loaded (rare, event driven), never per frame.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(LUMEN_HAS_PNG)
#include <png.h>
#endif
#if defined(LUMEN_HAS_RSVG)
#include <cairo.h>
#include <librsvg/rsvg.h>
#endif

import lx.foundation;
import lx.trace;

export module lx.shell:icon_theme;

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
    struct theme_index {
        std::string name;
        /// Base directories the theme exists in (…/icons/<name>).
        std::vector<std::string> roots;
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
    [[nodiscard]] const theme_index* index_of(const std::string& name);
    [[nodiscard]] std::string find_in(const theme_index& t, const char* name, int px) const;
    [[nodiscard]] static bool decode(const std::string& path, int px, std::vector<std::uint32_t>& out);

    std::string theme_;
    bool loaded_ = false;
    std::vector<std::string> bases_;
    std::vector<std::unique_ptr<theme_index>> themes_;
    std::vector<const theme_index*> chain_;
    std::vector<cached> cache_;
    unsigned long long clock_ = 0;
};

} // namespace lx::shell

namespace {

constexpr const char* k_icon_category = "shell.icon";

bool file_exists(const std::string& path) {
#if !defined(_WIN32)
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#else
    (void)path;
    return false;
#endif
}

bool dir_exists(const std::string& path) {
#if !defined(_WIN32)
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#else
    (void)path;
    return false;
#endif
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

void lx::shell::icon_theme::set_theme(const char* name) {
    theme_ = name ? name : "";
    loaded_ = false;
    themes_.clear();
    chain_.clear();
    cache_.clear();
}

const lx::shell::icon_theme::theme_index* lx::shell::icon_theme::index_of(const std::string& name) {
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
        t->roots.push_back(root);
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
                present = present || dir_exists(r + "/" + s.path);
            if (!present)
                continue;
            if (s.min_size <= 0)
                s.min_size = s.size;
            if (s.max_size <= 0)
                s.max_size = s.size;
            t->dirs.push_back(s);
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
        const theme_index* t = index_of(pending[i]);
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
    if (const theme_index* h = index_of("hicolor"))
        chain_.push_back(h);
    char line[256];
    std::snprintf(line, sizeof(line), "icon theme %s (%u themes in the chain; png=%d svg=%d)",
                  theme_.c_str(), static_cast<unsigned>(chain_.size()), can_png() ? 1 : 0,
                  can_svg() ? 1 : 0);
    lx::trace::logger::global().log(lx::trace::level::info, k_icon_category, line);
}

std::string lx::shell::icon_theme::find_in(const theme_index& t, const char* name, int px) const {
    // icon-theme-spec LookupIcon, in pixels (Size × Scale): an exact match first, else the
    // directory with the smallest size distance.
    const char* exts[] = {can_png() ? ".png" : nullptr, can_svg() ? ".svg" : nullptr};
    std::string best;
    int best_distance = 1 << 30;
    for (const auto& d : t.dirs) {
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
        for (const auto& r : t.roots) {
            for (const char* ext : exts) {
                if (!ext)
                    continue;
                std::string path = r + "/" + d.path + "/" + name + ext;
                if (!file_exists(path))
                    continue;
                if (match)
                    return path;
                best = path;
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
    for (const theme_index* t : chain_)
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

bool lx::shell::icon_theme::decode(const std::string& path, int px, std::vector<std::uint32_t>& out) {
    const bool svg = path.size() > 4 && path.compare(path.size() - 4, 4, ".svg") == 0;
    out.assign(static_cast<std::size_t>(px) * px, 0u);
    if (svg) {
#if defined(LUMEN_HAS_RSVG)
        GError* err = nullptr;
        RsvgHandle* h = rsvg_handle_new_from_file(path.c_str(), &err);
        if (!h) {
            if (err)
                g_error_free(err);
            return false;
        }
        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        cairo_t* cr = cairo_create(surf);
        const RsvgRectangle viewport{0.0, 0.0, static_cast<double>(px), static_cast<double>(px)};
        const bool ok = rsvg_handle_render_document(h, cr, &viewport, &err) != 0;
        if (err)
            g_error_free(err);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        // CAIRO_FORMAT_ARGB32 is premultiplied, native-endian 0xAARRGGBB: what the panel draws.
        const unsigned char* data = cairo_image_surface_get_data(surf);
        const int stride = cairo_image_surface_get_stride(surf);
        for (int y = 0; ok && data && y < px; ++y)
            std::memcpy(out.data() + static_cast<std::size_t>(y) * px, data + static_cast<std::size_t>(y) * stride,
                        static_cast<std::size_t>(px) * 4u);
        cairo_surface_destroy(surf);
        g_object_unref(h);
        return ok;
#else
        return false;
#endif
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
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        char line[512];
        std::snprintf(line, sizeof(line), "icon %s %dpx -> %s%s", name, px,
                      file.empty() ? "(not found)" : file.c_str(),
                      !file.empty() && entry.pixels.empty() ? " (undecodable)" : "");
        lx::trace::logger::global().log(lx::trace::level::debug, k_icon_category, line);
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
