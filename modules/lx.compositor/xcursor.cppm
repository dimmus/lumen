module;

#include <stdio.h>

import lx.foundation;

export module lx.compositor:xcursor;

import std;

export namespace lx::compositor {

/// Cursor shapes, numbered exactly as `wp_cursor_shape_device_v1.shape` so a protocol value
/// converts with a cast. `none` (0) is not a shape — it means "no override".
enum class cursor_shape : unsigned {
    none = 0,
    default_ = 1,
    context_menu,
    help,
    pointer,
    progress,
    wait,
    cell,
    crosshair,
    text,
    vertical_text,
    alias,
    copy,
    move,
    no_drop,
    not_allowed,
    grab,
    grabbing,
    e_resize,
    n_resize,
    ne_resize,
    nw_resize,
    s_resize,
    se_resize,
    sw_resize,
    w_resize,
    ew_resize,
    ns_resize,
    nesw_resize,
    nwse_resize,
    col_resize,
    row_resize,
    all_scroll,
    zoom_in,
    zoom_out,
    dnd_ask,
    all_resize,
};

inline constexpr unsigned k_cursor_shape_count = 37;

/// Resize cursor for an edge mask (top=1, bottom=2, left=4, right=8 — the xdg_toplevel
/// resize_edge bits). No edges is a move.
[[nodiscard]] cursor_shape shape_for_edges(unsigned edges);

/// CSS name of a shape (`default`, `nw-resize`, …), for logs and theme lookup.
[[nodiscard]] const char* cursor_shape_name(cursor_shape shape);

/// A premultiplied ARGB8888 image (native-endian words, so DRM_FORMAT_ARGB8888 in memory).
struct cursor_image_view {
    const std::uint32_t* pixels = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride_px = 0;
    int hot_x = 0;
    int hot_y = 0;

    [[nodiscard]] bool empty() const { return !pixels || width == 0 || height == 0; }
};

/// Cursor images for every shape: the user's Xcursor theme where it has them, and a
/// compiled-in vector set everywhere else, so there is always something to show.
///
/// Loaded once at startup (file I/O, UI thread, before the first frame); lookups after
/// that are a table index and allocate nothing.
class xcursor_theme {
public:
    /// `theme` null → $XCURSOR_THEME, else "default". `size` 0 → $XCURSOR_SIZE, else 24.
    /// Searches $XCURSOR_PATH, or the libXcursor default path, following `Inherits=`.
    void load(const char* theme, unsigned size);
    /// Overrides the search path (a colon list) for the next `load`; tests use it.
    void set_search_path(const char* colon_list);

    /// Never empty after `load`.
    [[nodiscard]] cursor_image_view image(cursor_shape shape) const;
    [[nodiscard]] unsigned from_theme() const { return from_theme_; }
    [[nodiscard]] unsigned builtin() const { return builtin_; }
    [[nodiscard]] unsigned size() const { return size_; }
    [[nodiscard]] const char* name() const { return name_.c_str(); }
    /// Largest width or height among the loaded images — what a cursor plane must hold.
    [[nodiscard]] unsigned max_dimension() const { return max_dimension_; }

private:
    struct entry {
        std::vector<std::uint32_t> pixels{};
        unsigned width = 0;
        unsigned height = 0;
        int hot_x = 0;
        int hot_y = 0;
    };

    entry entries_[k_cursor_shape_count]{};
    std::string name_{};
    std::string search_path_override_{};
    unsigned size_ = 0;
    unsigned from_theme_ = 0;
    unsigned builtin_ = 0;
    unsigned max_dimension_ = 0;
};

/// Parses an Xcursor file (untrusted input: every offset and size is bounds-checked).
/// Picks the image whose nominal size is closest to `want_size` (ties → the larger), first
/// frame only. Pixels are already premultiplied ARGB.
[[nodiscard]] bool parse_xcursor(const unsigned char* data, std::size_t len, unsigned want_size,
                                 std::vector<std::uint32_t>& out, unsigned& width,
                                 unsigned& height, int& hot_x, int& hot_y);

/// Rasterizes the built-in vector cursor for `shape` at `size` px: 4x4 supersampled, black
/// fill with a white outline, premultiplied ARGB. Shapes without their own drawing use the
/// nearest one (resize family → its arrow, the rest → the default arrow).
void rasterize_builtin(cursor_shape shape, unsigned size, std::vector<std::uint32_t>& out,
                       unsigned& width, unsigned& height, int& hot_x, int& hot_y);

} // namespace lx::compositor


namespace {

using lx::compositor::cursor_shape;

constexpr unsigned k_max_image_px = 256;
constexpr unsigned k_image_chunk = 0xfffd0002u;

// ── shape names ────────────────────────────────────────────────────────────

/// CSS name first, then the legacy X11 names themes still ship under.
struct shape_names {
    const char* names[5];
};

const shape_names k_names[lx::compositor::k_cursor_shape_count] = {
    {{nullptr}},
    {{"default", "left_ptr", nullptr}},
    {{"context-menu", nullptr}},
    {{"help", "question_arrow", "whats_this", "left_ptr_help", nullptr}},
    {{"pointer", "hand2", "hand1", "pointing_hand", nullptr}},
    {{"progress", "left_ptr_watch", "half-busy", nullptr}},
    {{"wait", "watch", nullptr}},
    {{"cell", "plus", nullptr}},
    {{"crosshair", "cross", "tcross", nullptr}},
    {{"text", "xterm", "ibeam", nullptr}},
    {{"vertical-text", nullptr}},
    {{"alias", "link", "dnd-link", nullptr}},
    {{"copy", "dnd-copy", nullptr}},
    {{"move", "fleur", "size_all", "all-scroll", nullptr}},
    {{"no-drop", "dnd-no-drop", nullptr}},
    {{"not-allowed", "crossed_circle", "forbidden", nullptr}},
    {{"grab", "openhand", "hand1", nullptr}},
    {{"grabbing", "closedhand", "fleur", nullptr}},
    {{"e-resize", "right_side", nullptr}},
    {{"n-resize", "top_side", nullptr}},
    {{"ne-resize", "top_right_corner", nullptr}},
    {{"nw-resize", "top_left_corner", nullptr}},
    {{"s-resize", "bottom_side", nullptr}},
    {{"se-resize", "bottom_right_corner", nullptr}},
    {{"sw-resize", "bottom_left_corner", nullptr}},
    {{"w-resize", "left_side", nullptr}},
    {{"ew-resize", "sb_h_double_arrow", "h_double_arrow", "size_hor", nullptr}},
    {{"ns-resize", "sb_v_double_arrow", "v_double_arrow", "size_ver", nullptr}},
    {{"nesw-resize", "fd_double_arrow", "size_bdiag", nullptr}},
    {{"nwse-resize", "bd_double_arrow", "size_fdiag", nullptr}},
    {{"col-resize", "split_h", "sb_h_double_arrow", nullptr}},
    {{"row-resize", "split_v", "sb_v_double_arrow", nullptr}},
    {{"all-scroll", "fleur", nullptr}},
    {{"zoom-in", nullptr}},
    {{"zoom-out", nullptr}},
    {{"dnd-ask", "copy", nullptr}},
    {{"all-resize", "fleur", nullptr}},
};

// ── Xcursor file format ────────────────────────────────────────────────────

[[nodiscard]] std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    constexpr long k_max_file = 16L * 1024 * 1024;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    bool ok = false;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long len = std::ftell(f);
        if (len > 0 && len <= k_max_file && std::fseek(f, 0, SEEK_SET) == 0) {
            out.resize(static_cast<std::size_t>(len));
            ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        }
    }
    std::fclose(f);
    return ok;
}

// ── theme search ───────────────────────────────────────────────────────────

void split_colon_list(const char* list, std::vector<std::string>& out) {
    if (!list)
        return;
    const char* start = list;
    for (const char* p = list;; ++p) {
        if (*p == ':' || *p == '\0') {
            if (p > start)
                out.emplace_back(start, static_cast<std::size_t>(p - start));
            if (*p == '\0')
                break;
            start = p + 1;
        }
    }
}

/// libXcursor's default search path, XDG-aware.
void default_search_path(std::vector<std::string>& out) {
    const char* home = std::getenv("HOME");
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home && *data_home)
        out.emplace_back(std::string{data_home} + "/icons");
    else if (home && *home)
        out.emplace_back(std::string{home} + "/.local/share/icons");
    if (home && *home)
        out.emplace_back(std::string{home} + "/.icons");
    const char* data_dirs = std::getenv("XDG_DATA_DIRS");
    std::vector<std::string> dirs;
    split_colon_list(data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share", dirs);
    for (auto& d : dirs)
        out.emplace_back(d + "/icons");
    out.emplace_back("/usr/share/pixmaps");
}

/// `Inherits=` of `theme`, from the first index.theme found along the search path.
void theme_inherits(const std::vector<std::string>& dirs, const std::string& theme,
                    std::vector<std::string>& out) {
    std::vector<unsigned char> text;
    for (const auto& dir : dirs) {
        if (!read_file(dir + "/" + theme + "/index.theme", text))
            continue;
        const std::string s(text.begin(), text.end());
        std::size_t pos = 0;
        while (pos < s.size()) {
            std::size_t eol = s.find('\n', pos);
            if (eol == std::string::npos)
                eol = s.size();
            const std::string line = s.substr(pos, eol - pos);
            pos = eol + 1;
            if (line.rfind("Inherits", 0) != 0)
                continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string name;
            for (std::size_t i = eq + 1; i <= line.size(); ++i) {
                const char c = i < line.size() ? line[i] : ',';
                if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r') {
                    if (!name.empty())
                        out.push_back(name);
                    name.clear();
                } else {
                    name.push_back(c);
                }
            }
        }
        return; // libXcursor stops at the first index.theme
    }
}

/// Lookup order: the theme, then what it inherits, depth first (as libXcursor does),
/// bounded against cycles and runaway chains.
void build_theme_chain(const std::vector<std::string>& dirs, const std::string& theme,
                       unsigned depth, std::vector<std::string>& chain) {
    constexpr unsigned k_max_depth = 8;
    constexpr std::size_t k_max_themes = 16;
    if (depth > k_max_depth || chain.size() >= k_max_themes)
        return;
    for (const auto& seen : chain)
        if (seen == theme)
            return;
    chain.push_back(theme);
    std::vector<std::string> parents;
    theme_inherits(dirs, theme, parents);
    for (const auto& parent : parents)
        build_theme_chain(dirs, parent, depth + 1, chain);
}

// ── built-in vector shapes ─────────────────────────────────────────────────
//
// Drawn on a 32-unit design grid and rasterized at the requested size, so the fallback is
// as crisp at XCURSOR_SIZE=48 as at 24 — a bitmap fallback would be scaled and blurry.

struct pt {
    float x;
    float y;
};

constexpr unsigned k_max_polys = 6;
constexpr unsigned k_max_points = 8;

struct poly {
    pt p[k_max_points];
    unsigned n;
};

struct vector_shape {
    std::inplace_vector<poly, k_max_polys> polys;
    pt hot{16.f, 16.f};

    void add(std::initializer_list<pt> points) {
        if (polys.size() >= k_max_polys)
            return;
        poly& q = polys.emplace_back(); // value-initialized: n == 0
        for (const pt& v : points)
            if (q.n < k_max_points)
                q.p[q.n++] = v;
    }
    void add_rect(float x0, float y0, float x1, float y1) {
        add({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    }
    /// Clockwise on screen (y grows downward), about the grid centre.
    void rotate(float degrees) {
        const float r = degrees * 3.14159265358979f / 180.f;
        const float c = std::cos(r);
        const float s = std::sin(r);
        for (poly& q : polys) {
            for (unsigned k = 0; k < q.n; ++k) {
                const float dx = q.p[k].x - 16.f;
                const float dy = q.p[k].y - 16.f;
                q.p[k] = {16.f + dx * c - dy * s, 16.f + dx * s + dy * c};
            }
        }
    }
};

enum class builtin_kind { arrow, four_way, edge_arrow, double_arrow, ibeam };

struct builtin_choice {
    builtin_kind kind;
    float degrees;
};

[[nodiscard]] builtin_choice builtin_for(cursor_shape shape) {
    switch (shape) {
    case cursor_shape::move:
    case cursor_shape::grab:
    case cursor_shape::grabbing:
    case cursor_shape::all_scroll:
    case cursor_shape::all_resize: return {builtin_kind::four_way, 0.f};
    case cursor_shape::n_resize: return {builtin_kind::edge_arrow, 0.f};
    case cursor_shape::ne_resize: return {builtin_kind::edge_arrow, 45.f};
    case cursor_shape::e_resize: return {builtin_kind::edge_arrow, 90.f};
    case cursor_shape::se_resize: return {builtin_kind::edge_arrow, 135.f};
    case cursor_shape::s_resize: return {builtin_kind::edge_arrow, 180.f};
    case cursor_shape::sw_resize: return {builtin_kind::edge_arrow, 225.f};
    case cursor_shape::w_resize: return {builtin_kind::edge_arrow, 270.f};
    case cursor_shape::nw_resize: return {builtin_kind::edge_arrow, 315.f};
    case cursor_shape::ns_resize:
    case cursor_shape::row_resize: return {builtin_kind::double_arrow, 0.f};
    case cursor_shape::nesw_resize: return {builtin_kind::double_arrow, 45.f};
    case cursor_shape::ew_resize:
    case cursor_shape::col_resize: return {builtin_kind::double_arrow, 90.f};
    case cursor_shape::nwse_resize: return {builtin_kind::double_arrow, 135.f};
    case cursor_shape::text: return {builtin_kind::ibeam, 0.f};
    case cursor_shape::vertical_text: return {builtin_kind::ibeam, 90.f};
    default: return {builtin_kind::arrow, 0.f};
    }
}

[[nodiscard]] vector_shape make_shape(builtin_choice choice) {
    vector_shape s{};
    switch (choice.kind) {
    case builtin_kind::arrow:
        // The classic left_ptr; the hotspot is the tip.
        s.add({{2.f, 2.f},
               {2.f, 24.f},
               {7.2f, 19.2f},
               {10.6f, 27.f},
               {14.f, 25.6f},
               {10.7f, 17.9f},
               {17.8f, 17.9f}});
        s.hot = {2.f, 2.f};
        break;
    case builtin_kind::edge_arrow:
        // Arrowhead pointing at a bar: "this edge moves". Drawn for north, then rotated.
        s.add_rect(8.f, 4.f, 24.f, 6.5f);
        s.add({{16.f, 8.5f}, {22.5f, 15.f}, {9.5f, 15.f}});
        s.add_rect(14.6f, 14.5f, 17.4f, 27.f);
        break;
    case builtin_kind::double_arrow:
        s.add({{16.f, 3.f}, {22.5f, 10.f}, {9.5f, 10.f}});
        s.add({{16.f, 29.f}, {9.5f, 22.f}, {22.5f, 22.f}});
        s.add_rect(14.6f, 9.5f, 17.4f, 22.5f);
        break;
    case builtin_kind::four_way:
        s.add({{16.f, 2.f}, {21.f, 8.f}, {11.f, 8.f}});
        s.add({{16.f, 30.f}, {11.f, 24.f}, {21.f, 24.f}});
        s.add({{2.f, 16.f}, {8.f, 11.f}, {8.f, 21.f}});
        s.add({{30.f, 16.f}, {24.f, 21.f}, {24.f, 11.f}});
        s.add_rect(14.75f, 7.5f, 17.25f, 24.5f);
        s.add_rect(7.5f, 14.75f, 24.5f, 17.25f);
        break;
    case builtin_kind::ibeam:
        s.add_rect(11.f, 4.f, 21.f, 6.4f);
        s.add_rect(14.8f, 6.f, 17.2f, 26.f);
        s.add_rect(11.f, 25.6f, 21.f, 28.f);
        break;
    }
    if (choice.degrees != 0.f)
        s.rotate(choice.degrees);
    return s;
}

[[nodiscard]] bool inside_poly(const poly& q, float x, float y) {
    bool in = false;
    for (unsigned i = 0, j = q.n - 1; i < q.n; j = i++) {
        const pt a = q.p[i];
        const pt b = q.p[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

[[nodiscard]] float dist2_to_segment(pt a, pt b, float x, float y) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.f ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    const float px = a.x + t * dx - x;
    const float py = a.y + t * dy - y;
    return px * px + py * py;
}

void rasterize(const vector_shape& s, unsigned size, std::vector<std::uint32_t>& out) {
    const float scale = static_cast<float>(size) / 32.f;
    // A 1.5 px outline at the common sizes (one design unit once that is larger): thinner
    // and the antialiased band never reaches full white, so the pointer vanishes on dark
    // content.
    const float r = 1.5f / scale > 1.f ? 1.5f / scale : 1.f;
    const float r2 = r * r;
    out.assign(static_cast<std::size_t>(size) * size, 0u);
    constexpr unsigned k_ss = 4;
    for (unsigned py = 0; py < size; ++py) {
        for (unsigned px = 0; px < size; ++px) {
            unsigned fill = 0;
            unsigned outline = 0;
            for (unsigned sy = 0; sy < k_ss; ++sy) {
                for (unsigned sx = 0; sx < k_ss; ++sx) {
                    const float ux = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / k_ss) / scale;
                    const float uy = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / k_ss) / scale;
                    const bool in = std::ranges::any_of(
                        s.polys, [&](const poly& q) { return inside_poly(q, ux, uy); });
                    if (in) {
                        ++fill;
                        continue;
                    }
                    bool near = false;
                    for (const poly& q : s.polys) {
                        for (unsigned a = 0, b = q.n - 1; a < q.n && !near; b = a++)
                            near = dist2_to_segment(q.p[a], q.p[b], ux, uy) <= r2;
                        if (near)
                            break;
                    }
                    if (near)
                        ++outline;
                }
            }
            constexpr unsigned k_samples = k_ss * k_ss;
            const unsigned alpha = ((fill + outline) * 255u + k_samples / 2) / k_samples;
            const unsigned white = (outline * 255u + k_samples / 2) / k_samples; // premultiplied
            out[static_cast<std::size_t>(py) * size + px] =
                (alpha << 24) | (white << 16) | (white << 8) | white;
        }
    }
}

} // namespace

// ── public helpers ─────────────────────────────────────────────────────────

lx::compositor::cursor_shape lx::compositor::shape_for_edges(unsigned edges) {
    constexpr unsigned top = 1, bottom = 2, left = 4, right = 8;
    switch (edges) {
    case top: return cursor_shape::n_resize;
    case bottom: return cursor_shape::s_resize;
    case left: return cursor_shape::w_resize;
    case right: return cursor_shape::e_resize;
    case top | left: return cursor_shape::nw_resize;
    case top | right: return cursor_shape::ne_resize;
    case bottom | left: return cursor_shape::sw_resize;
    case bottom | right: return cursor_shape::se_resize;
    default: return cursor_shape::move;
    }
}

const char* lx::compositor::cursor_shape_name(cursor_shape shape) {
    const auto i = static_cast<unsigned>(shape);
    if (i == 0 || i >= k_cursor_shape_count)
        return "none";
    return k_names[i].names[0];
}

bool lx::compositor::parse_xcursor(const unsigned char* data, std::size_t len,
                                   unsigned want_size, std::vector<std::uint32_t>& out,
                                   unsigned& width, unsigned& height, int& hot_x, int& hot_y) {
    constexpr std::size_t k_file_header = 16;
    constexpr std::size_t k_toc_entry = 12;
    constexpr std::size_t k_chunk_header = 36;
    constexpr std::uint32_t k_max_toc = 1024;
    if (!data || len < k_file_header || std::memcmp(data, "Xcur", 4) != 0)
        return false;
    const std::uint32_t header = le32(data + 4);
    const std::uint32_t ntoc = le32(data + 12);
    if (header < k_file_header || ntoc == 0 || ntoc > k_max_toc ||
        header + static_cast<std::size_t>(ntoc) * k_toc_entry > len)
        return false;

    std::uint32_t best = 0;
    std::uint32_t best_dist = ~0u;
    bool any = false;
    for (std::uint32_t i = 0; i < ntoc; ++i) {
        const unsigned char* e = data + header + static_cast<std::size_t>(i) * k_toc_entry;
        if (le32(e) != k_image_chunk)
            continue;
        const std::uint32_t nominal = le32(e + 4);
        const std::uint32_t dist = nominal > want_size ? nominal - want_size : want_size - nominal;
        if (!any || dist < best_dist || (dist == best_dist && nominal > best)) {
            best = nominal;
            best_dist = dist;
            any = true;
        }
    }
    if (!any)
        return false;

    // First image of that size: animated cursors carry several frames per size.
    for (std::uint32_t i = 0; i < ntoc; ++i) {
        const unsigned char* e = data + header + static_cast<std::size_t>(i) * k_toc_entry;
        if (le32(e) != k_image_chunk || le32(e + 4) != best)
            continue;
        const std::size_t pos = le32(e + 8);
        if (pos > len || len - pos < k_chunk_header)
            return false;
        const unsigned char* c = data + pos;
        const std::uint32_t chunk_header = le32(c);
        if (le32(c + 4) != k_image_chunk || chunk_header < k_chunk_header)
            return false;
        const std::uint32_t w = le32(c + 16);
        const std::uint32_t h = le32(c + 20);
        const std::uint32_t xhot = le32(c + 24);
        const std::uint32_t yhot = le32(c + 28);
        if (w == 0 || h == 0 || w > k_max_image_px || h > k_max_image_px || xhot > w || yhot > h)
            return false;
        const std::size_t bytes = static_cast<std::size_t>(w) * h * 4u;
        if (len - pos < chunk_header || len - pos - chunk_header < bytes)
            return false;
        const unsigned char* px = c + chunk_header;
        out.resize(static_cast<std::size_t>(w) * h);
        for (std::size_t k = 0; k < out.size(); ++k)
            out[k] = le32(px + k * 4u);
        width = w;
        height = h;
        hot_x = static_cast<int>(xhot < w ? xhot : w - 1);
        hot_y = static_cast<int>(yhot < h ? yhot : h - 1);
        return true;
    }
    return false;
}

void lx::compositor::rasterize_builtin(cursor_shape shape, unsigned size,
                                       std::vector<std::uint32_t>& out, unsigned& width,
                                       unsigned& height, int& hot_x, int& hot_y) {
    if (size < 8)
        size = 8;
    if (size > k_max_image_px)
        size = k_max_image_px;
    const vector_shape s = make_shape(builtin_for(shape));
    rasterize(s, size, out);
    width = size;
    height = size;
    const float scale = static_cast<float>(size) / 32.f;
    hot_x = static_cast<int>(s.hot.x * scale);
    hot_y = static_cast<int>(s.hot.y * scale);
    if (hot_x >= static_cast<int>(size))
        hot_x = static_cast<int>(size) - 1;
    if (hot_y >= static_cast<int>(size))
        hot_y = static_cast<int>(size) - 1;
}

// ── xcursor_theme ──────────────────────────────────────────────────────────

void lx::compositor::xcursor_theme::set_search_path(const char* colon_list) {
    search_path_override_ = colon_list ? colon_list : "";
}

void lx::compositor::xcursor_theme::load(const char* theme, unsigned size) {
    if (!theme || !*theme) {
        const char* env = std::getenv("XCURSOR_THEME");
        theme = env && *env ? env : "default";
    }
    if (size == 0) {
        const char* env = std::getenv("XCURSOR_SIZE");
        const long parsed = env ? std::strtol(env, nullptr, 10) : 0;
        size = parsed > 0 && parsed <= static_cast<long>(k_max_image_px)
                   ? static_cast<unsigned>(parsed)
                   : 24u;
    }
    name_ = theme;
    size_ = size;
    from_theme_ = 0;
    builtin_ = 0;
    max_dimension_ = 0;

    std::vector<std::string> dirs;
    if (!search_path_override_.empty()) {
        split_colon_list(search_path_override_.c_str(), dirs);
    } else if (const char* env = std::getenv("XCURSOR_PATH"); env && *env) {
        split_colon_list(env, dirs);
    } else {
        default_search_path(dirs);
    }
    std::vector<std::string> chain;
    build_theme_chain(dirs, name_, 0, chain);

    std::vector<unsigned char> file;
    for (unsigned i = 1; i < k_cursor_shape_count; ++i) {
        entry& e = entries_[i];
        bool found = false;
        for (unsigned n = 0; !found && n < 5 && k_names[i].names[n]; ++n) {
            for (const auto& t : chain) {
                for (const auto& dir : dirs) {
                    if (!read_file(dir + "/" + t + "/cursors/" + k_names[i].names[n], file))
                        continue;
                    found = parse_xcursor(file.data(), file.size(), size, e.pixels, e.width,
                                          e.height, e.hot_x, e.hot_y);
                    if (found)
                        break;
                }
                if (found)
                    break;
            }
        }
        if (found) {
            ++from_theme_;
        } else {
            rasterize_builtin(static_cast<cursor_shape>(i), size, e.pixels, e.width, e.height,
                              e.hot_x, e.hot_y);
            ++builtin_;
        }
        if (e.width > max_dimension_)
            max_dimension_ = e.width;
        if (e.height > max_dimension_)
            max_dimension_ = e.height;
    }
}

lx::compositor::cursor_image_view lx::compositor::xcursor_theme::image(cursor_shape shape) const {
    unsigned i = static_cast<unsigned>(shape);
    if (i == 0 || i >= k_cursor_shape_count)
        i = std::to_underlying(cursor_shape::default_);
    const entry& e = entries_[i];
    cursor_image_view v{};
    v.pixels = e.pixels.empty() ? nullptr : e.pixels.data();
    v.width = e.width;
    v.height = e.height;
    v.stride_px = e.width;
    v.hot_x = e.hot_x;
    v.hot_y = e.hot_y;
    return v;
}
