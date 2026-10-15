module;

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

import lx.foundation;
import lx.trace;
import lx.scene;
import lx.text;

export module lx.compositor:decoration;

import :toplevel;
import :surface_tree;
import :xcursor;
import :cursor;

export namespace lx::compositor {

// ── Frame geometry (scale 1) ───────────────────────────────────────────────

/// Title bar height.
inline constexpr int k_deco_title_height = 28;
/// Visible border around the client area (none when maximized).
inline constexpr int k_deco_border = 1;
/// Invisible resize band outside the visible frame (Mutter style): a 1 px border is not a
/// target anyone can hit.
inline constexpr int k_deco_resize_margin = 8;
/// Length along a resize band, from a corner, that resizes diagonally.
inline constexpr int k_deco_corner = 20;
/// Space kept free on each side of the title text.
inline constexpr int k_deco_title_padding = 10;

/// The shared atlas (swatches, close glyph, button circles). Outside the shm range
/// (0x4000'0000 upward) and apart from the cursor's id.
inline constexpr unsigned k_deco_atlas_texture_id = 0x7FFF'FE00u;
inline constexpr unsigned k_deco_atlas_size = 64;
/// Per-window title textures are allocated upward from here, never reused: a dated
/// retirement of an id must not land on a later texture with the same id.
inline constexpr unsigned k_deco_title_texture_base = 0x7F00'0000u;

/// Visible frame around the client's window geometry.
struct frame_extents {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    [[nodiscard]] bool operator==(const frame_extents&) const = default;
    [[nodiscard]] bool empty() const { return !left && !top && !right && !bottom; }
};

/// Server-side: 1 px borders and a 28 px title bar above the top border's inner edge;
/// maximized: the title bar only; client-side: nothing.
[[nodiscard]] frame_extents extents_for(decoration_mode mode, bool maximized);
/// `geometry` (the client's window geometry, global) grown by the extents.
[[nodiscard]] lx::rect2i frame_rect(lx::rect2i geometry, frame_extents e);

/// Parts of a server-side frame under a point.
enum class deco_zone : unsigned char {
    none,
    title,
    close,
    top,
    bottom,
    left,
    right,
    top_left,
    top_right,
    bottom_left,
    bottom_right,
};

/// The zone of the frame at global (gx, gy), or none. The close button first, then the
/// title bar, then the resize bands — the visible border plus the invisible margin outside
/// it — where a point within `k_deco_corner` of a corner along a band picks the corner.
/// Points inside the client geometry are never a zone: the client's surfaces are hit first,
/// and what they leave uncovered there is nobody's. Maximized frames have no resize bands.
[[nodiscard]] deco_zone deco_hit(lx::rect2i geometry, frame_extents e, double gx, double gy);
/// `resize_edge_bits` of a band zone; 0 for none, title and close.
[[nodiscard]] unsigned edges_for_zone(deco_zone zone);
/// Pointer shape over a zone: the resize shapes on the bands, the default elsewhere.
[[nodiscard]] cursor_shape cursor_for_zone(deco_zone zone);
[[nodiscard]] const char* deco_zone_name(deco_zone zone);

/// Title bar rect (global) of a frame around `geometry`.
[[nodiscard]] lx::rect2i title_rect(lx::rect2i geometry);
/// Close button rect (global): the title bar's right end, a T x T square.
[[nodiscard]] lx::rect2i close_rect(lx::rect2i geometry);
/// Width left for the title text in a frame `geometry_width` wide.
[[nodiscard]] int title_text_room(int geometry_width);

// ── Atlas ──────────────────────────────────────────────────────────────────

/// Where each piece sits in the 64x64 atlas. Fills sample a swatch's centre 2x2
/// (`swatch_centre`), stretched over the target: the GL path's half-texel inset keeps
/// linear filtering inside the swatch and CPU nearest sampling is exact, so the colour is
/// the baked one on every backend (no tint, whose semantics differ between backends).
namespace deco_atlas {
inline constexpr lx::rect2i title_active{0, 0, 4, 4};
inline constexpr lx::rect2i title_inactive{4, 0, 4, 4};
inline constexpr lx::rect2i border_active{8, 0, 4, 4};
inline constexpr lx::rect2i border_inactive{12, 0, 4, 4};
inline constexpr lx::rect2i close_light{0, 8, 10, 10};
inline constexpr lx::rect2i close_dim{16, 8, 10, 10};
inline constexpr lx::rect2i circle_hover{0, 24, 20, 20};
inline constexpr lx::rect2i circle_pressed{24, 24, 20, 20};
} // namespace deco_atlas

[[nodiscard]] constexpr lx::rect2i swatch_centre(lx::rect2i swatch) {
    return {swatch.x + 1, swatch.y + 1, 2, 2};
}

/// Fills `out` (k_deco_atlas_size², premultiplied ARGB) with the atlas: colour swatches, the
/// close glyph (analytic anti-aliasing, 1.5 px strokes) and the button circles.
void build_deco_atlas(std::uint32_t* out);

// ── Scene node ─────────────────────────────────────────────────────────────

enum class deco_button : unsigned char { normal, hover, pressed };

/// The frame of one window: the underlay of its tree container, so it restacks with the
/// window and paints below its surfaces. 7–8 draws: four border fills, the title fill, the
/// title text, the close button (circle when hovered or pressed, glyph).
class frame_node final : public scene::scene_node {
public:
    ~frame_node() override;
    [[nodiscard]] scene::node_kind kind() const override;
    void emit_draws(scene::draw_list& out) const override;

    lx::rect2i geometry{};
    frame_extents extents{};
    bool shown = false;
    bool active = false;
    deco_button button = deco_button::normal;
    /// Title textures (active text colour, inactive), padded by `title_pad` on each side.
    unsigned title_texture[2]{};
    lx::size2i title_size{};
    int title_pad = 0;
    /// Width of the drawn text (the fit, ellipsis included), for centring.
    int title_text_width = 0;
    bool title_ready = false;
    /// The atlas has been staged; nothing is drawn before (no draw of an unknown texture).
    const bool* atlas_ready = nullptr;
};

// ── Manager ────────────────────────────────────────────────────────────────

/// Server-side frames. UI affinity. One slot per decorated toplevel: the frame node, its
/// title textures and the title's fit, re-rasterized only when the title changes or the
/// ellipsis cut moves with the width (a focus change swaps texture ids; a move costs no
/// texture work at all). Damage is the frame's strips (title bar, borders), never the
/// client area, which the window's own nodes already account for.
class decoration_manager {
public:
    static constexpr unsigned k_max = toplevel_manager::k_max;

    void bind(surface_tree& tree, scene::scene_graph& scene);
    void set_texture_ops(compositor_texture_ops ops) { ops_ = ops; }

    /// Brings the frame of `rec` in line with it: shown iff mapped with the server-side mode
    /// applied, at its geometry, with the maximized extents when maximized. Damages old and
    /// new strips when anything moved. O(1) apart from a slot lookup; no allocation.
    void sync(const toplevel_record& rec);
    /// The title changed (xdg_toplevel.set_title). Rasterized at the next `on_tick`.
    void on_title_changed(const toplevel_record& rec);
    /// The window that owns the keyboard gets the active colours (null: none).
    void set_active(lx::toplevel_id id);
    /// Close-button look (hover / pressed). Damages the button only.
    void set_button(lx::toplevel_id id, deco_button state);
    /// Damages the whole visible frame (the window was restacked).
    void damage(lx::toplevel_id id);
    /// The toplevel record is going: frame off the tree, textures retired, slot freed.
    void on_destroyed(lx::toplevel_id id, lx::surface_id root);
    /// Once per frame tick, before the scene is published: stages the atlas the first time a
    /// frame is shown, rasterizes changed titles.
    void on_tick();

    [[nodiscard]] bool shown(lx::toplevel_id id) const;
    [[nodiscard]] const frame_node* node(lx::toplevel_id id) const;

private:
    struct slot {
        lx::toplevel_id id{};
        lx::surface_id root{};
        bool used = false;
        /// The node is the underlay of the root's tree container.
        bool installed = false;
        frame_node node{};
        char title[256]{};
        /// The title text changed: rasterize regardless of the fit.
        bool title_changed = true;
        /// The width changed: rasterize only if the ellipsis cut moved.
        bool refit = true;
        unsigned glyph_end = 0;
        bool ellipsized = false;
        bool textures_staged = false;
    };

    [[nodiscard]] slot* find(lx::toplevel_id id);
    [[nodiscard]] const slot* find(lx::toplevel_id id) const;
    [[nodiscard]] slot* alloc(const toplevel_record& rec);
    void damage_strips(const frame_node& n) const;
    void damage_title(const frame_node& n) const;
    void raster_title(slot& s);
    void log(const char* line) const;

    surface_tree* tree_ = nullptr;
    scene::scene_graph* scene_ = nullptr;
    compositor_texture_ops ops_{};
    slot slots_[k_max]{};
    lx::toplevel_id active_{};
    unsigned next_texture_id_ = k_deco_title_texture_base;
    bool atlas_ready_ = false;
    bool any_dirty_ = false;
    bool warned_no_text_ = false;
    /// Scratch, reused: the shaped title and the raster buffer (grown, never shrunk).
    lx::text::shaped_line line_{};
    std::vector<std::uint32_t> raster_{};
};

} // namespace lx::compositor


namespace {

constexpr const char* k_deco_category = "compositor.deco";

[[nodiscard]] bool deco_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

[[nodiscard]] bool rect_eq(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] bool in_rect(lx::rect2i r, double x, double y) {
    return x >= r.x && y >= r.y && x < static_cast<double>(r.x) + r.width &&
           y < static_cast<double>(r.y) + r.height;
}

/// Premultiplied ARGB from straight sRGB components and a coverage in [0, 1].
[[nodiscard]] std::uint32_t premul(std::uint32_t rgb, double coverage) {
    if (coverage <= 0.0)
        return 0u;
    if (coverage > 1.0)
        coverage = 1.0;
    const auto ch = [&](unsigned shift) {
        return static_cast<std::uint32_t>(((rgb >> shift) & 0xFFu) * coverage + 0.5);
    };
    const auto a = static_cast<std::uint32_t>(255.0 * coverage + 0.5);
    return (a << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

/// Distance from (px, py) to the segment (ax, ay)–(bx, by).
[[nodiscard]] double segment_distance(double px, double py, double ax, double ay, double bx,
                                      double by) {
    const double dx = bx - ax, dy = by - ay;
    double t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double cx = ax + t * dx - px, cy = ay + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

// Palette (sRGB).
constexpr std::uint32_t k_title_active = 0x303033u;
constexpr std::uint32_t k_title_inactive = 0x48484Cu;
constexpr std::uint32_t k_border_active = 0x1B1B1Du;
constexpr std::uint32_t k_border_inactive = 0x2C2C30u;
constexpr std::uint32_t k_button_hover = 0xE0443Eu;
constexpr std::uint32_t k_button_pressed = 0xB8322Du;
constexpr std::uint32_t k_glyph_light = 0xF2F2F2u;
constexpr std::uint32_t k_glyph_dim = 0xA8A8ACu;
constexpr lx::text::raster_color k_text_active{0xF2, 0xF2, 0xF2, 0xFF};
constexpr lx::text::raster_color k_text_inactive{0xA8, 0xA8, 0xAC, 0xFF};
/// One font per process: lx.text caches a single face (R12).
constexpr lx::text::font_desc k_title_font{"Sans", 10.f, lx::text::weight::bold,
                                           lx::text::style::normal};

void push_draw(lx::scene::draw_list& out, unsigned texture, lx::rect2i src, lx::rect2i dst,
               lx::rect2i clip, lx::blend_mode blend) {
    if (dst.width <= 0 || dst.height <= 0)
        return;
    lx::scene::draw_command cmd{};
    cmd.texture = lx::texture_id{texture};
    cmd.src = src;
    cmd.dst = dst;
    cmd.clip = clip;
    cmd.opacity = 1.f;
    cmd.tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    cmd.blend = blend;
    cmd.sort_key = lx::draw_sort_key::make(0, out.size());
    out.push(cmd);
}

} // namespace

// ── Pure helpers ───────────────────────────────────────────────────────────

lx::compositor::frame_extents lx::compositor::extents_for(decoration_mode mode, bool maximized) {
    if (mode != decoration_mode::server)
        return {};
    if (maximized)
        return {0, k_deco_title_height, 0, 0};
    return {k_deco_border, k_deco_title_height + k_deco_border, k_deco_border, k_deco_border};
}

lx::rect2i lx::compositor::frame_rect(lx::rect2i g, frame_extents e) {
    return {g.x - e.left, g.y - e.top, g.width + e.left + e.right, g.height + e.top + e.bottom};
}

lx::rect2i lx::compositor::title_rect(lx::rect2i g) {
    return {g.x, g.y - k_deco_title_height, g.width, k_deco_title_height};
}

lx::rect2i lx::compositor::close_rect(lx::rect2i g) {
    return {g.x + g.width - k_deco_title_height, g.y - k_deco_title_height, k_deco_title_height,
            k_deco_title_height};
}

int lx::compositor::title_text_room(int geometry_width) {
    return geometry_width - 2 * k_deco_title_padding - k_deco_title_height;
}

lx::compositor::deco_zone lx::compositor::deco_hit(lx::rect2i g, frame_extents e, double gx,
                                                   double gy) {
    if (e.empty() || g.width <= 0 || g.height <= 0)
        return deco_zone::none;
    const lx::rect2i close = close_rect(g);
    const lx::rect2i title = title_rect(g);
    if (in_rect(close, gx, gy) && in_rect(title, gx, gy))
        return deco_zone::close;
    if (in_rect(title, gx, gy))
        return deco_zone::title;
    // Maximized: the title bar is all there is.
    if (!e.left && !e.right && !e.bottom)
        return deco_zone::none;
    const lx::rect2i f = frame_rect(g, e);
    const int m = k_deco_resize_margin;
    const lx::rect2i outer{f.x - m, f.y - m, f.width + 2 * m, f.height + 2 * m};
    if (!in_rect(outer, gx, gy) || in_rect(g, gx, gy))
        return deco_zone::none;
    // The bands: everything in the outer rect that is neither client nor title bar.
    const bool left = gx < g.x;
    const bool right = gx >= static_cast<double>(g.x) + g.width;
    const bool top = gy < title.y;
    const bool bottom = gy >= static_cast<double>(g.y) + g.height;
    const int k = k_deco_corner;
    const bool near_left = gx < static_cast<double>(f.x) + k;
    const bool near_right = gx >= static_cast<double>(f.x) + f.width - k;
    const bool near_top = gy < static_cast<double>(f.y) + k;
    const bool near_bottom = gy >= static_cast<double>(f.y) + f.height - k;
    if ((top && near_left) || (left && near_top))
        return deco_zone::top_left;
    if ((top && near_right) || (right && near_top))
        return deco_zone::top_right;
    if ((bottom && near_left) || (left && near_bottom))
        return deco_zone::bottom_left;
    if ((bottom && near_right) || (right && near_bottom))
        return deco_zone::bottom_right;
    if (top)
        return deco_zone::top;
    if (bottom)
        return deco_zone::bottom;
    if (left)
        return deco_zone::left;
    if (right)
        return deco_zone::right;
    return deco_zone::none;
}

unsigned lx::compositor::edges_for_zone(deco_zone zone) {
    switch (zone) {
    case deco_zone::top:
        return edge_top;
    case deco_zone::bottom:
        return edge_bottom;
    case deco_zone::left:
        return edge_left;
    case deco_zone::right:
        return edge_right;
    case deco_zone::top_left:
        return edge_top | edge_left;
    case deco_zone::top_right:
        return edge_top | edge_right;
    case deco_zone::bottom_left:
        return edge_bottom | edge_left;
    case deco_zone::bottom_right:
        return edge_bottom | edge_right;
    case deco_zone::none:
    case deco_zone::title:
    case deco_zone::close:
        break;
    }
    return edge_none;
}

lx::compositor::cursor_shape lx::compositor::cursor_for_zone(deco_zone zone) {
    const unsigned edges = edges_for_zone(zone);
    return edges ? shape_for_edges(edges) : cursor_shape::default_;
}

const char* lx::compositor::deco_zone_name(deco_zone zone) {
    switch (zone) {
    case deco_zone::none:
        return "none";
    case deco_zone::title:
        return "title";
    case deco_zone::close:
        return "close";
    case deco_zone::top:
        return "top";
    case deco_zone::bottom:
        return "bottom";
    case deco_zone::left:
        return "left";
    case deco_zone::right:
        return "right";
    case deco_zone::top_left:
        return "top_left";
    case deco_zone::top_right:
        return "top_right";
    case deco_zone::bottom_left:
        return "bottom_left";
    case deco_zone::bottom_right:
        return "bottom_right";
    }
    return "none";
}

void lx::compositor::build_deco_atlas(std::uint32_t* out) {
    const unsigned n = k_deco_atlas_size;
    for (unsigned i = 0; i < n * n; ++i)
        out[i] = 0u;
    const auto fill = [&](lx::rect2i r, std::uint32_t rgb) {
        for (int y = r.y; y < r.y + r.height; ++y)
            for (int x = r.x; x < r.x + r.width; ++x)
                out[static_cast<unsigned>(y) * n + static_cast<unsigned>(x)] = premul(rgb, 1.0);
    };
    fill(deco_atlas::title_active, k_title_active);
    fill(deco_atlas::title_inactive, k_title_inactive);
    fill(deco_atlas::border_active, k_border_active);
    fill(deco_atlas::border_inactive, k_border_inactive);

    // "×": two diagonals with a 1.5 px stroke, coverage from the pixel centre's distance to
    // the stroke's centre line (a 1 px linear ramp at the edge).
    const auto glyph = [&](lx::rect2i r, std::uint32_t rgb) {
        for (int y = 0; y < r.height; ++y)
            for (int x = 0; x < r.width; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double d1 = segment_distance(px, py, 1.5, 1.5, 8.5, 8.5);
                const double d2 = segment_distance(px, py, 8.5, 1.5, 1.5, 8.5);
                const double d = d1 < d2 ? d1 : d2;
                const double cov = 0.75 + 0.5 - d; // half stroke + half pixel - distance
                out[static_cast<unsigned>(r.y + y) * n + static_cast<unsigned>(r.x + x)] =
                    premul(rgb, cov);
            }
    };
    glyph(deco_atlas::close_light, k_glyph_light);
    glyph(deco_atlas::close_dim, k_glyph_dim);

    const auto circle = [&](lx::rect2i r, std::uint32_t rgb) {
        const double c = r.width / 2.0;
        for (int y = 0; y < r.height; ++y)
            for (int x = 0; x < r.width; ++x) {
                const double dx = x + 0.5 - c, dy = y + 0.5 - c;
                const double cov = c + 0.5 - std::sqrt(dx * dx + dy * dy);
                out[static_cast<unsigned>(r.y + y) * n + static_cast<unsigned>(r.x + x)] =
                    premul(rgb, cov);
            }
    };
    circle(deco_atlas::circle_hover, k_button_hover);
    circle(deco_atlas::circle_pressed, k_button_pressed);
}

// ── frame_node ─────────────────────────────────────────────────────────────

lx::compositor::frame_node::~frame_node() = default;

lx::scene::node_kind lx::compositor::frame_node::kind() const {
    // Not a container, so `scene_graph::emit_node` asks it for draws.
    return scene::node_kind::surface;
}

void lx::compositor::frame_node::emit_draws(scene::draw_list& out) const {
    if (!shown || !atlas_ready || !*atlas_ready || geometry.width <= 0 || geometry.height <= 0)
        return;
    const lx::rect2i g = geometry;
    const lx::rect2i f = frame_rect(g, extents);
    const unsigned atlas = k_deco_atlas_texture_id;
    const lx::rect2i border =
        swatch_centre(active ? deco_atlas::border_active : deco_atlas::border_inactive);
    // Borders: left and right full height, top and bottom between them.
    if (extents.left > 0)
        push_draw(out, atlas, border, {f.x, f.y, extents.left, f.height}, {},
                  lx::blend_mode::opaque);
    if (extents.right > 0)
        push_draw(out, atlas, border, {g.x + g.width, f.y, extents.right, f.height}, {},
                  lx::blend_mode::opaque);
    if (extents.top > k_deco_title_height)
        push_draw(out, atlas, border,
                  {g.x, f.y, g.width, extents.top - k_deco_title_height}, {},
                  lx::blend_mode::opaque);
    if (extents.bottom > 0)
        push_draw(out, atlas, border, {g.x, g.y + g.height, g.width, extents.bottom}, {},
                  lx::blend_mode::opaque);

    const lx::rect2i title = title_rect(g);
    push_draw(out, atlas,
              swatch_centre(active ? deco_atlas::title_active : deco_atlas::title_inactive),
              title, {}, lx::blend_mode::opaque);

    if (title_ready && title_size.width > 0 && title_size.height > 0) {
        const int room = title_text_room(g.width);
        const int x0 = title.x + k_deco_title_padding;
        const int x = x0 + (room > title_text_width ? (room - title_text_width) / 2 : 0);
        const int y = title.y + (k_deco_title_height - title_size.height) / 2;
        push_draw(out, title_texture[active ? 0 : 1], {0, 0, title_size.width, title_size.height},
                  {x - title_pad, y, title_size.width, title_size.height},
                  {title.x, title.y, g.width - k_deco_title_height, k_deco_title_height},
                  lx::blend_mode::premultiplied);
    }

    const lx::rect2i close = close_rect(g);
    if (button != deco_button::normal) {
        const lx::rect2i c =
            button == deco_button::pressed ? deco_atlas::circle_pressed : deco_atlas::circle_hover;
        push_draw(out, atlas, c,
                  {close.x + (close.width - c.width) / 2, close.y + (close.height - c.height) / 2,
                   c.width, c.height},
                  title, lx::blend_mode::premultiplied);
    }
    const lx::rect2i glyph = button != deco_button::normal || active ? deco_atlas::close_light
                                                                     : deco_atlas::close_dim;
    push_draw(out, atlas, glyph,
              {close.x + (close.width - glyph.width) / 2,
               close.y + (close.height - glyph.height) / 2, glyph.width, glyph.height},
              title, lx::blend_mode::premultiplied);
}

// ── decoration_manager ─────────────────────────────────────────────────────

void lx::compositor::decoration_manager::bind(surface_tree& tree, scene::scene_graph& scene) {
    tree_ = &tree;
    scene_ = &scene;
}

void lx::compositor::decoration_manager::log(const char* line) const {
    lx::trace::logger::global().log(lx::trace::level::debug, k_deco_category, line);
}

lx::compositor::decoration_manager::slot*
lx::compositor::decoration_manager::find(lx::toplevel_id id) {
    if (!id)
        return nullptr;
    for (auto& s : slots_)
        if (s.used && s.id == id)
            return &s;
    return nullptr;
}

const lx::compositor::decoration_manager::slot*
lx::compositor::decoration_manager::find(lx::toplevel_id id) const {
    if (!id)
        return nullptr;
    for (const auto& s : slots_)
        if (s.used && s.id == id)
            return &s;
    return nullptr;
}

lx::compositor::decoration_manager::slot*
lx::compositor::decoration_manager::alloc(const toplevel_record& rec) {
    for (auto& s : slots_) {
        if (s.used)
            continue;
        // The node stays where it is (it is not in any container now); only its state resets.
        s.id = rec.id;
        s.root = rec.surface;
        s.used = true;
        s.installed = false;
        s.title[0] = '\0';
        s.title_changed = true;
        s.refit = true;
        s.glyph_end = 0;
        s.ellipsized = false;
        s.textures_staged = false;
        s.node.geometry = {};
        s.node.extents = {};
        s.node.shown = false;
        s.node.active = rec.id == active_;
        s.node.button = deco_button::normal;
        s.node.title_size = {};
        s.node.title_pad = 0;
        s.node.title_text_width = 0;
        s.node.title_ready = false;
        s.node.atlas_ready = &atlas_ready_;
        // Fresh ids per window, two at a time; wrapping takes 16M windows.
        if (next_texture_id_ + 2 >= k_deco_atlas_texture_id)
            next_texture_id_ = k_deco_title_texture_base;
        s.node.title_texture[0] = next_texture_id_++;
        s.node.title_texture[1] = next_texture_id_++;
        return &s;
    }
    return nullptr;
}

void lx::compositor::decoration_manager::damage_strips(const frame_node& n) const {
    if (!scene_ || !n.shown)
        return;
    const lx::rect2i g = n.geometry;
    const frame_extents e = n.extents;
    const lx::rect2i f = frame_rect(g, e);
    // The frame without the client area: four strips, never one window-sized rect.
    if (e.top > 0)
        scene_->note_damage({f.x, f.y, f.width, e.top});
    if (e.bottom > 0)
        scene_->note_damage({f.x, g.y + g.height, f.width, e.bottom});
    if (e.left > 0)
        scene_->note_damage({f.x, g.y, e.left, g.height});
    if (e.right > 0)
        scene_->note_damage({g.x + g.width, g.y, e.right, g.height});
}

void lx::compositor::decoration_manager::damage_title(const frame_node& n) const {
    if (scene_ && n.shown)
        scene_->note_damage(title_rect(n.geometry));
}

void lx::compositor::decoration_manager::sync(const toplevel_record& rec) {
    const bool want = rec.mapped && rec.decoration_current == decoration_mode::server;
    slot* s = find(rec.id);
    if (!want) {
        if (!s)
            return;
        damage_strips(s->node);
        s->node.shown = false;
        if (s->installed && tree_)
            tree_->set_underlay(s->root, nullptr);
        s->installed = false;
        return;
    }
    if (!s) {
        s = alloc(rec);
        if (!s) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                lx::trace::logger::global().log(lx::trace::level::warn, k_deco_category,
                                                "decoration table full — frame not drawn. "
                                                "Reported once.");
            }
            return;
        }
    }
    const char* title = rec.title ? rec.title : "";
    if (std::strncmp(s->title, title, sizeof(s->title) - 1) != 0) {
        std::snprintf(s->title, sizeof(s->title), "%s", title);
        s->title_changed = true;
        any_dirty_ = true;
    }
    if (!s->installed && tree_) {
        tree_->set_underlay(s->root, &s->node);
        s->installed = true;
    }
    const frame_extents ext =
        extents_for(decoration_mode::server, rec.state == lx::toplevel_state::maximized);
    if (s->node.shown && rect_eq(s->node.geometry, rec.geometry) && s->node.extents == ext)
        return;
    const bool width_changed = !s->node.shown || s->node.geometry.width != rec.geometry.width;
    damage_strips(s->node);
    s->node.geometry = rec.geometry;
    s->node.extents = ext;
    s->node.shown = true;
    damage_strips(s->node);
    if (width_changed) {
        s->refit = true;
        any_dirty_ = true;
    }
}

void lx::compositor::decoration_manager::on_title_changed(const toplevel_record& rec) {
    slot* s = find(rec.id);
    if (!s)
        return; // copied when the frame is first shown
    const char* title = rec.title ? rec.title : "";
    if (std::strncmp(s->title, title, sizeof(s->title) - 1) == 0)
        return;
    std::snprintf(s->title, sizeof(s->title), "%s", title);
    s->title_changed = true;
    any_dirty_ = true;
}

void lx::compositor::decoration_manager::set_active(lx::toplevel_id id) {
    if (id == active_)
        return;
    slot* old = find(active_);
    if (old) {
        old->node.active = false;
        damage_strips(old->node);
    }
    const lx::toplevel_id previous = active_;
    active_ = id;
    slot* s = find(id);
    if (s) {
        s->node.active = true;
        damage_strips(s->node);
    }
    if ((old || s) && deco_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "decoration active id=%llu previous=%llu", id.id(),
                      previous.id());
        log(line);
    }
}

void lx::compositor::decoration_manager::set_button(lx::toplevel_id id, deco_button state) {
    slot* s = find(id);
    if (!s || s->node.button == state)
        return;
    s->node.button = state;
    if (scene_ && s->node.shown)
        scene_->note_damage(close_rect(s->node.geometry));
}

void lx::compositor::decoration_manager::damage(lx::toplevel_id id) {
    if (const slot* s = find(id))
        damage_strips(s->node);
}

void lx::compositor::decoration_manager::on_destroyed(lx::toplevel_id id, lx::surface_id root) {
    slot* s = find(id);
    if (!s)
        return;
    damage_strips(s->node);
    s->node.shown = false;
    // The surface (and its container) may already be gone; set_underlay ignores that.
    if (s->installed && tree_)
        tree_->set_underlay(root, nullptr);
    if (s->textures_staged && ops_.retire) {
        ops_.retire(ops_.user, s->node.title_texture[0]);
        ops_.retire(ops_.user, s->node.title_texture[1]);
    }
    s->used = false;
    s->installed = false;
    s->id = {};
    s->root = {};
    if (active_ == id)
        active_ = {};
}

bool lx::compositor::decoration_manager::shown(lx::toplevel_id id) const {
    const slot* s = find(id);
    return s && s->node.shown;
}

const lx::compositor::frame_node*
lx::compositor::decoration_manager::node(lx::toplevel_id id) const {
    const slot* s = find(id);
    return s ? &s->node : nullptr;
}

void lx::compositor::decoration_manager::raster_title(slot& s) {
    const bool changed = s.title_changed;
    s.title_changed = false;
    s.refit = false;
    frame_node& n = s.node;
    const int room = title_text_room(n.geometry.width);

    const auto drop = [&] {
        if (n.title_ready)
            damage_title(n);
        n.title_ready = false;
        s.glyph_end = 0;
        s.ellipsized = false;
    };
    if (room <= 0 || s.title[0] == '\0') {
        drop();
        return;
    }
    if (auto shaped = lx::text::shape_line(s.title, k_title_font, line_); !shaped) {
        if (!warned_no_text_) {
            warned_no_text_ = true;
            lx::trace::logger::global().log(lx::trace::level::warn, k_deco_category,
                                            "no title text: lx.text cannot shape (no font "
                                            "face, or built without HarfBuzz). Reported once.");
        }
        drop();
        return;
    }
    const lx::text::line_fit fit = lx::text::fit_line(line_, room);
    if (fit.width_px <= 0) {
        drop();
        return;
    }
    // A resize that leaves the cut where it was costs nothing: same pixels, same texture.
    if (!changed && n.title_ready && fit.glyph_end == s.glyph_end &&
        fit.ellipsized == s.ellipsized)
        return;

    constexpr int k_pad = 2; // glyph ink may overhang the advance box
    const int w = fit.width_px + 2 * k_pad;
    const int h = line_.ascent_px + line_.descent_px;
    if (w <= 0 || h <= 0 || !ops_.stage) {
        drop();
        return;
    }
    const std::size_t px = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (raster_.size() < px)
        raster_.resize(px); // grows to the widest title seen, then stays
    const lx::text::raster_color colours[2] = {k_text_active, k_text_inactive};
    for (unsigned i = 0; i < 2; ++i) {
        std::memset(raster_.data(), 0, px * sizeof(std::uint32_t));
        (void)lx::text::render_line(line_, fit, colours[i], {raster_.data(), w, h, w},
                                    {k_pad, line_.ascent_px});
        if (!ops_.stage(ops_.user, n.title_texture[i], raster_.data(), static_cast<unsigned>(w),
                        static_cast<unsigned>(h), static_cast<unsigned>(w),
                        static_cast<unsigned>(w), static_cast<unsigned>(h))) {
            // Staging full this frame: try again next tick, with the old title meanwhile.
            s.title_changed = changed;
            s.refit = true;
            any_dirty_ = true;
            return;
        }
    }
    s.textures_staged = true;
    damage_title(n); // old extent
    n.title_size = {w, h};
    n.title_pad = k_pad;
    n.title_text_width = fit.width_px;
    n.title_ready = true;
    s.glyph_end = fit.glyph_end;
    s.ellipsized = fit.ellipsized;
    damage_title(n);
    if (deco_debug()) {
        char line[320];
        std::snprintf(line, sizeof(line),
                      "decoration title id=%llu width=%d ellipsized=%d title=\"%.200s\"",
                      s.id.id(), fit.width_px, fit.ellipsized ? 1 : 0, s.title);
        log(line);
    }
}

void lx::compositor::decoration_manager::on_tick() {
    if (!atlas_ready_) {
        bool any_shown = false;
        for (const auto& s : slots_)
            any_shown = any_shown || (s.used && s.node.shown);
        if (any_shown && ops_.stage) {
            // Generated once, lazily: by the first decorated map the present backend and its
            // staging are up. 16 KiB on the stack, once per process.
            std::uint32_t atlas[k_deco_atlas_size * k_deco_atlas_size];
            build_deco_atlas(atlas);
            if (ops_.stage(ops_.user, k_deco_atlas_texture_id, atlas, k_deco_atlas_size,
                           k_deco_atlas_size, k_deco_atlas_size, k_deco_atlas_size,
                           k_deco_atlas_size)) {
                atlas_ready_ = true;
                // Frames shown before now drew nothing: their pixels are new.
                for (const auto& s : slots_)
                    if (s.used)
                        damage_strips(s.node);
            }
        }
    }
    if (!any_dirty_)
        return;
    any_dirty_ = false;
    for (auto& s : slots_) {
        if (!s.used || !s.node.shown || !(s.title_changed || s.refit))
            continue;
        raster_title(s);
    }
}
