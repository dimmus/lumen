module;

#include <math.h>

import lx.foundation;
import lx.gfx;
import lx.trace;
import lx.scene;
import lx.text;

export module lx.compositor:decoration;

import std;

import :toplevel;
import :surface_tree;
import :xcursor;
import :cursor;
import :ui_composite;

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
/// The close glyph's edge at scale 1 (its swatch in the decoration atlas, the tile in the
/// compositor's text atlas).
inline constexpr int k_deco_close_glyph = 10;

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

/// The close glyph's coverage at `scale` (`size` = round(10 × scale) texels square, R8, `size`
/// bytes per row): the "×" of `build_deco_atlas` — two diagonals, 1.5 px strokes, analytic
/// anti-aliasing — drawn for that scale rather than resampled. At scale 1 its bytes are the
/// alpha of the atlas swatch.
void build_close_coverage(double scale, int size, std::uint8_t* out);

// ── Title text (ADR-010 §4.11) ─────────────────────────────────────────────

/// Distinct output scales a window's title is kept rasterised for at once; on an output of
/// another scale the largest is resampled.
inline constexpr unsigned k_title_scales = 4;
/// Distinct scales the close glyph is kept for.
inline constexpr unsigned k_close_glyph_scales = 8;

/// One glyph of a rasterised title: its tile and its box relative to the text's origin (the
/// pen's start on the baseline), device pixels at the run's scale.
struct title_glyph {
    gfx::atlas_tile_id tile{};
    std::int32_t x = 0;
    /// The box's top from the baseline, down positive (above the baseline: negative).
    std::int32_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    /// An RGBA8 tile (a colour glyph): drawn as a `poly_sprite`.
    bool color = false;
};

/// A title laid out and rasterised at one scale, drawn 1:1 on outputs of that scale.
struct title_run {
    /// 0: unused.
    double scale = 0.0;
    /// What is drawn — the title's fit (ellipsis included) and the not-responding suffix —
    /// in device pixels; 0: nothing.
    int width = 0;
    int ascent = 0;
    int descent = 0;
    /// The title's fit, so a resize that cuts where it did lays nothing out.
    unsigned glyph_end = 0;
    bool ellipsized = false;
    bool not_responding = false;
    /// Grown to the longest title seen at this scale, then reused: a title change allocates
    /// at most once; a frame never does.
    std::vector<title_glyph> glyphs{};
};

/// A window's title at the scales of the outputs it is on.
struct title_text {
    title_run runs[k_title_scales]{};
    /// The run drawn on an output of `scale`: the one rasterised for it, else the largest
    /// (resampled); null when there is none.
    [[nodiscard]] const title_run* for_scale(double scale) const;
};

/// The close button's "×" at one scale: a tile of `build_close_coverage`.
struct close_glyph {
    double scale = 0.0;
    gfx::atlas_tile_id tile{};
    int size = 0;
};

/// The title font at scale 1: "Sans", bold, 10 pt — the CPU titles' font.
inline constexpr text::font_desc k_title_font{"Sans", 10.f, text::weight::bold, text::style::normal};

/// Server-side frames' text in the compositor's atlas (`chrome_atlas`): titles shaped and
/// fitted by `lx.text` (the single-line API: one face, the ellipsis and the not-responding
/// rule of the CPU-rasterised titles it replaces), their glyphs rasterised by an
/// `lx.text::glyph_cache` over the chrome atlas — 4 subpixel variants, light hinting below
/// scale 1.5 — and the close glyph. UI affinity. Rasterising takes the atlas's mutex, and only
/// on a miss; a frame's emission looks tiles up without it.
class chrome_text {
public:
    explicit chrome_text(chrome_atlas& atlas);
    ~chrome_text();
    chrome_text(const chrome_text&) = delete;
    chrome_text& operator=(const chrome_text&) = delete;

    [[nodiscard]] chrome_atlas& atlas() { return *atlas_; }
    [[nodiscard]] text::glyph_cache& glyphs() { return *glyphs_; }
    /// The title font at scale 1 (default `k_title_font`).
    void set_font(text::font_desc font) { font_ = font; }
    [[nodiscard]] text::font_desc font() const { return font_; }

    /// Lays `title` out in `room_logical` logical pixels at `scale` into `run`: shaped at the
    /// font's size × scale, ellipsized to the room (with `not_responding`, " (Not Responding)"
    /// always shown and the title fitted to what it leaves, as KWin does), every glyph placed
    /// on a quarter pixel and rasterised into the atlas if it is not there. `changed`: the text
    /// is new — otherwise a run whose fit is unchanged is left as it is. True when the run's
    /// pixels changed. Errors: lx.text cannot shape (no face, no HarfBuzz).
    [[nodiscard]] lx::result<bool> layout_title(title_run& run, const char* title,
                                                bool not_responding, int room_logical,
                                                double scale, bool changed);
    /// The close glyph at `scale`, rasterised on first use; null when the atlas refuses it.
    const close_glyph* close(double scale);
    /// The close glyph at `scale` if it is rasterised (emission: no raster).
    [[nodiscard]] const close_glyph* find_close(double scale) const;

    /// An emitter found a tile gone (the atlas evicted it): the manager lays the titles out
    /// again at its next tick.
    void note_stale() { stale_ = true; }
    [[nodiscard]] bool take_stale() {
        const bool s = stale_;
        stale_ = false;
        return s;
    }

private:
    [[nodiscard]] bool add_glyph(title_run& run, text::face_id face, std::int32_t size,
                                 text::glyph_hinting hinting, std::uint32_t glyph,
                                 std::int32_t pen_x, std::int32_t x_off, std::int32_t y_off);

    chrome_atlas* atlas_ = nullptr;
    std::unique_ptr<text::glyph_cache> glyphs_{};
    text::font_desc font_ = k_title_font;
    /// Scratch, allocated once: the shaped title, the suffix, the ellipsis.
    std::unique_ptr<text::shaped_line> line_{};
    std::unique_ptr<text::shaped_line> suffix_{};
    std::unique_ptr<text::shaped_line> ellipsis_{};
    close_glyph close_[k_close_glyph_scales]{};
    unsigned next_close_ = 0;
    bool stale_ = false;
};

// ── Scene node ─────────────────────────────────────────────────────────────

enum class deco_button : unsigned char { normal, hover, pressed };

/// The frame of one window: the underlay of its tree container, so it restacks with the
/// window and paints below its surfaces. The four border fills and the title fill are UI
/// primitives (ADR-010 §4.11: radius-0 solid quads, the palette's colours, the projected
/// rects the swatch blits had); the title text and the close glyph are `mono_sprite`s from the
/// compositor's text atlas (`text`), drawn 1:1 on an output whose scale the title was
/// rasterised for (the manager keeps one run per output scale the frame is on) and resampled
/// from the largest run elsewhere; the button's hover / pressed circle stays a blit of the
/// decoration atlas. A dimmed frame's fills and glyphs carry the dim already applied, the way
/// `cpu_compositor` tints an encoded byte (`(c · 166) >> 8` for 0.65) — the pixels the tinted
/// swatch blit had on the CPU backend, and one colour rule on every backend. Without `text`
/// (a node built by hand) the close glyph is the decoration atlas's blit and no title is
/// drawn.
class frame_node final : public scene::scene_node {
public:
    ~frame_node() override;
    [[nodiscard]] scene::node_kind kind() const override;
    using scene::scene_node::emit_draws;
    /// Rects are laid out in logical coordinates and projected relative to the window's
    /// tree anchor, so the frame's edges round with the client's (no seam at 1.25).
    void emit_draws(scene::draw_list& out, const scene::emit_context& ctx) const override;

    lx::rect2i geometry{};
    frame_extents extents{};
    bool shown = false;
    bool active = false;
    deco_button button = deco_button::normal;
    /// The window's title, laid out per output scale (the manager's); null: no title.
    const title_text* title = nullptr;
    /// The compositor's text atlas the title and close glyphs are tiles of; null: no glyph
    /// sprites (the close glyph falls back to the decoration atlas's blit).
    chrome_text* text = nullptr;
    /// The client stopped answering pings: the frame is drawn through the same grey tint
    /// as the window's surfaces.
    bool dimmed = false;
    /// The atlas has been staged; nothing is drawn before (no draw of an unknown texture).
    const bool* atlas_ready = nullptr;
};

// ── Manager ────────────────────────────────────────────────────────────────

/// Server-side frames. UI affinity. One slot per decorated toplevel: the frame node and its
/// title laid out per output scale, re-laid out only when the title changes, the ellipsis cut
/// moves with the width or the window's scales change (a focus change only recolours the
/// sprites; a move costs no text work at all). Glyphs go to the compositor's atlas through
/// `chrome_text` (`bind_chrome`). Damage is the frame's strips (title bar, borders), never
/// the client area, which the window's own nodes already account for.
class decoration_manager {
public:
    static constexpr unsigned k_max = toplevel_manager::k_max;

    void bind(surface_tree& tree, scene::scene_graph& scene);
    void set_texture_ops(compositor_texture_ops ops) { ops_ = ops; }
    /// The compositor's text atlas the titles and close glyphs are rasterised into (the render
    /// thread's UI renderer draws from it). Without one no title is drawn.
    void bind_chrome(chrome_atlas* atlas);
    [[nodiscard]] chrome_text* text() { return text_.get(); }

    /// Brings the frame of `rec` in line with it: shown iff mapped with the server-side mode
    /// applied, at its geometry, with the maximized extents when maximized. Damages old and
    /// new strips when anything moved. O(1) apart from a slot lookup; no allocation.
    void sync(const toplevel_record& rec);
    /// The title changed (xdg_toplevel.set_title). Rasterized at the next `on_tick`.
    void on_title_changed(const toplevel_record& rec);
    /// The window that owns the keyboard gets the active colours (null: none).
    void set_active(lx::toplevel_id id);
    /// The client stopped (or resumed) answering pings: the title reads
    /// "<title> (Not Responding)" — the suffix always visible, the title part ellipsized to
    /// the room left (KWin) — and the frame is dimmed. Rasterized at the next `on_tick`.
    void set_not_responding(lx::toplevel_id id, bool on);
    /// Close-button look (hover / pressed). Damages the button only.
    void set_button(lx::toplevel_id id, deco_button state);
    /// Damages the whole visible frame (the window was restacked).
    void damage(lx::toplevel_id id);
    /// The toplevel record is going: frame off the tree, textures retired, slot freed.
    void on_destroyed(lx::toplevel_id id, lx::surface_id root);
    /// Once per frame tick, before the scene is published: stages the atlas the first time a
    /// frame is shown, rasterizes changed titles.
    void on_tick();
    /// The scale titles of new frames are rasterized at (text at 10 pt × scale, in output
    /// pixels) until their outputs are known.
    void set_scale(double scale);
    /// Window `id`'s title at one scale (an output of another is resampled from it).
    void set_window_scale(lx::toplevel_id id, double scale);
    /// The scales of the outputs window `id`'s frame is on: its title is laid out at each
    /// (the first `k_title_scales` distinct ones), drawn 1:1 on every one of them. A change
    /// lays it out again at the next `on_tick` (at most once a tick however often it moves).
    void set_window_scales(lx::toplevel_id id, std::span<const double> scales);

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
        bool not_responding = false;
        /// The scales its title is laid out at (`scale_count` of them).
        double scales[k_title_scales]{1.0};
        unsigned scale_count = 1;
        title_text text{};
    };

    /// `slot*`, or `const slot*` on a const manager.
    [[nodiscard]] auto find(this auto& self, lx::toplevel_id id) -> decltype(&self.slots_[0]) {
        if (!id)
            return nullptr;
        for (auto& s : self.slots_)
            if (s.used && s.id == id)
                return &s;
        return nullptr;
    }
    [[nodiscard]] slot* alloc(const toplevel_record& rec);
    void damage_strips(const frame_node& n) const;
    void damage_title(const frame_node& n) const;
    void raster_title(slot& s);

    surface_tree* tree_ = nullptr;
    scene::scene_graph* scene_ = nullptr;
    compositor_texture_ops ops_{};
    /// Allocated 16 slots at a time (`lx::sparse_slots`): a slot holds its frame node and a
    /// 256-byte title, and the whole table was resident from start-up.
    lx::sparse_slots<slot, k_max> slots_{};
    lx::toplevel_id active_{};
    bool atlas_ready_ = false;
    bool any_dirty_ = false;
    bool warned_no_text_ = false;
    std::unique_ptr<chrome_text> text_{};
    double scale_ = 1.0;
};

} // namespace lx::compositor


namespace {

constexpr lx::trace::category log_deco{"compositor.deco"};

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

/// The close glyph's coverage at pixel (x, y) of its box at `scale`: two diagonals from
/// (1.5, 1.5) to (8.5, 8.5) of a 10 px box, a 1.5 px stroke, all scaled; a 1 px linear ramp at
/// the edge (half stroke + half pixel − distance). Unclamped.
[[nodiscard]] double close_coverage(double scale, int x, int y) {
    const double px = x + 0.5, py = y + 0.5;
    const double lo = 1.5 * scale, hi = 8.5 * scale;
    const double d1 = segment_distance(px, py, lo, lo, hi, hi);
    const double d2 = segment_distance(px, py, hi, lo, lo, hi);
    const double d = d1 < d2 ? d1 : d2;
    return 0.75 * scale + 0.5 - d;
}

// Palette (sRGB).
constexpr std::uint32_t k_title_active = 0x303033u;
constexpr std::uint32_t k_title_inactive = 0x48484Cu;
constexpr std::uint32_t k_border_active = 0x1B1B1Du;
constexpr std::uint32_t k_border_inactive = 0x2C2C30u;
constexpr std::uint32_t k_button_hover = 0xE0443Eu;
constexpr std::uint32_t k_button_pressed = 0xB8322Du;
/// The close glyph's colours, and the title text's (active, inactive).
constexpr std::uint32_t k_glyph_light = 0xF2F2F2u;
constexpr std::uint32_t k_glyph_dim = 0xA8A8ACu;

/// Appended to the title of a window whose client does not answer pings (KWin's wording).
constexpr const char* k_not_responding_suffix = " (Not Responding)";
/// The tint of a not-responding window, as `surface_tree::set_root_dim` uses for its
/// surfaces.
constexpr lx::color k_dim_tint = lx::color::rgb(0.65f, 0.65f, 0.65f, 1.f);
constexpr lx::color k_no_tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);

/// A fill colour of the palette as a frame's quad carries it: the sRGB bytes as encoded
/// values (the compositor's primitives blend in the backend's encoded space, ADR-010 §4.5),
/// dimmed with `cpu_compositor`'s byte rule for the dim tint.
[[nodiscard]] lx::color deco_fill_color(std::uint32_t rgb, bool dimmed) {
    // `cpu_compositor`'s `to_255(0.65)`: the tint as the byte multiplier it applies.
    const auto tint_255 = static_cast<std::uint32_t>(k_dim_tint.r * 255.f + 0.5f);
    const auto channel = [&](unsigned shift) {
        std::uint32_t v = (rgb >> shift) & 0xFFu;
        if (dimmed)
            v = (v * tint_255) >> 8u;
        return static_cast<float>(v) / 255.f;
    };
    return lx::color::rgb(channel(16), channel(8), channel(0), 1.f);
}

void push_draw(lx::scene::draw_list& out, unsigned texture, lx::rect2i src, lx::rect2i dst,
               lx::rect2i clip, lx::blend_mode blend, lx::color tint = k_no_tint,
               bool scaled = false) {
    if (dst.width <= 0 || dst.height <= 0)
        return;
    lx::scene::draw_command cmd{};
    cmd.texture = lx::texture_id{texture};
    cmd.src = src;
    cmd.dst = dst;
    cmd.scaled = scaled;
    cmd.clip = clip;
    cmd.opacity = 1.f;
    cmd.tint = tint;
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
            for (int x = 0; x < r.width; ++x)
                out[static_cast<unsigned>(r.y + y) * n + static_cast<unsigned>(r.x + x)] =
                    premul(rgb, close_coverage(1.0, x, y));
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

void lx::compositor::build_close_coverage(double scale, int size, std::uint8_t* out) {
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double cov = close_coverage(scale, x, y);
            cov = cov < 0.0 ? 0.0 : (cov > 1.0 ? 1.0 : cov);
            // The atlas swatch's alpha byte (`premul`'s rounding).
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
                static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(255.0 * cov + 0.5);
        }
}

// ── Title text ─────────────────────────────────────────────────────────────

const lx::compositor::title_run* lx::compositor::title_text::for_scale(double scale) const {
    const title_run* largest = nullptr;
    for (const title_run& r : runs) {
        if (!(r.scale > 0.0))
            continue;
        if (r.scale == scale)
            return &r;
        if (!largest || r.scale > largest->scale)
            largest = &r;
    }
    return largest;
}

lx::compositor::chrome_text::chrome_text(chrome_atlas& atlas) : atlas_{&atlas} {
    text::glyph_cache_config config{};
    // Titles touch a few hundred glyphs per scale; the tile ids the runs keep must not be
    // replaced under them, so the table is far from full.
    config.entries = 4096;
    config.max_glyph_px = 128;
    glyphs_ = std::make_unique<text::glyph_cache>(atlas.atlas(), config);
    line_ = std::make_unique<text::shaped_line>();
    suffix_ = std::make_unique<text::shaped_line>();
    ellipsis_ = std::make_unique<text::shaped_line>();
}

lx::compositor::chrome_text::~chrome_text() = default;

const lx::compositor::close_glyph* lx::compositor::chrome_text::find_close(double scale) const {
    for (const close_glyph& c : close_)
        if (c.scale == scale && c.tile.valid())
            return &c;
    return nullptr;
}

const lx::compositor::close_glyph* lx::compositor::chrome_text::close(double scale) {
    if (!(scale > 0.0))
        return nullptr;
    gfx::atlas_tile tile{};
    for (close_glyph& c : close_)
        if (c.scale == scale && c.tile.valid()) {
            if (atlas_->atlas().lookup(c.tile, tile))
                return &c;
            c = {}; // evicted: rasterised again below
        }
    const int size = std::max(1, static_cast<int>(std::lround(k_deco_close_glyph * scale)));
    if (size > static_cast<int>(text::k_glyph_max_px))
        return nullptr;
    std::uint8_t coverage[text::k_glyph_max_px * text::k_glyph_max_px];
    build_close_coverage(scale, size, coverage);
    lx::result<gfx::atlas_tile_id> id = lx::invalid_argument("chrome_text: no tile");
    {
        const std::lock_guard<std::mutex> lock{atlas_->mutex()};
        id = atlas_->atlas().insert(gfx::atlas_format::r8, static_cast<unsigned>(size),
                                    static_cast<unsigned>(size),
                                    {coverage, static_cast<std::size_t>(size) * size},
                                    static_cast<unsigned>(size));
    }
    if (!id)
        return nullptr;
    // A free entry, else round robin (the replaced tile ages out of the atlas on its own).
    close_glyph* slot = nullptr;
    for (close_glyph& c : close_)
        if (!c.tile.valid() && !slot)
            slot = &c;
    if (!slot) {
        slot = &close_[next_close_];
        next_close_ = (next_close_ + 1u) % k_close_glyph_scales;
    }
    *slot = {scale, id.value(), size};
    return slot;
}

bool lx::compositor::chrome_text::add_glyph(title_run& run, text::face_id face, std::int32_t size,
                                            text::glyph_hinting hinting, std::uint32_t glyph,
                                            std::int32_t pen_x, std::int32_t x_off,
                                            std::int32_t y_off) {
    // The pen on a quarter pixel (its variant rasterised), the baseline on a whole one.
    const text::glyph_pen_pos pos = text::glyph_pen(pen_x + x_off, y_off);
    const text::glyph_key key{face, size, glyph, pos.variant, hinting};
    const text::glyph_entry* e = glyphs_->find(key);
    if (!e) {
        // A miss rasterises into the atlas's pages: the render thread is not reading them.
        const std::lock_guard<std::mutex> lock{atlas_->mutex()};
        auto got = glyphs_->get(text::face_cache::shared(), key);
        if (!got)
            return false;
        e = got.value();
    }
    if (!e || e->empty())
        return true;
    title_glyph g{};
    g.tile = e->tile;
    g.x = pos.x + e->left;
    g.y = -pos.y - e->top;
    g.width = e->width;
    g.height = e->height;
    g.color = e->color;
    run.glyphs.push_back(g); // capacity kept: allocates only for a title longer than any before
    return true;
}

lx::result<bool> lx::compositor::chrome_text::layout_title(title_run& run, const char* title,
                                                          bool not_responding, int room_logical,
                                                          double scale, bool changed) {
    static constexpr const char* k_suffix = " (Not Responding)";
    static constexpr const char* k_ellipsis = "\xE2\x80\xA6";
    const auto clear = [&] {
        const bool had = run.width > 0;
        run.scale = scale;
        run.width = 0;
        run.glyph_end = 0;
        run.ellipsized = false;
        run.not_responding = not_responding;
        run.glyphs.clear();
        return had;
    };
    text::font_desc f = font_;
    f.size_pt = static_cast<float>(f.size_pt * scale);
    const int room = static_cast<int>(std::lround(room_logical * scale));
    const bool has_title = title && title[0] != '\0';
    // A not-responding window shows the suffix whatever the title's length: the title part
    // gets what room the suffix leaves (KWin).
    int suffix_w = 0;
    if (not_responding) {
        if (auto r = text::shape_line(k_suffix, f, *suffix_); !r)
            return r.get_error();
        suffix_w = suffix_->width_px;
    }
    if (room <= 0 || (!has_title && !not_responding))
        return clear();
    text::line_fit fit{};
    if (has_title) {
        if (auto r = text::shape_line(title, f, *line_); !r)
            return r.get_error();
        fit = text::fit_line(*line_, room - suffix_w);
    }
    // The suffix alone when the title gets no room; then it may be ellipsized itself.
    text::line_fit suffix_fit{};
    if (not_responding)
        suffix_fit = text::fit_line(*suffix_, fit.width_px > 0 ? suffix_w : room);
    const int text_w = fit.width_px + suffix_fit.width_px;
    if (text_w <= 0)
        return clear();
    // A resize that leaves the cut where it was costs nothing: same glyphs, same places.
    if (!changed && run.scale == scale && run.width > 0 && run.not_responding == not_responding &&
        fit.glyph_end == run.glyph_end && fit.ellipsized == run.ellipsized && !not_responding)
        return false;

    const text::face_id face = text::face_cache::shared().resolve(f);
    if (!face.valid())
        return lx::make_error(lx::io_err::not_found, "no title font face resolved");
    const std::int32_t size = text::size_26_6_from_pt(f.size_pt);
    const text::glyph_hinting hinting =
        text::effective_hinting(text::glyph_hinting::light, static_cast<float>(scale));
    if (fit.ellipsized || suffix_fit.ellipsized)
        if (auto r = text::shape_line(k_ellipsis, f, *ellipsis_); !r)
            return r.get_error();
    run.glyphs.clear();
    bool ok = true;
    // `render_line`'s placement: the kept prefix glyph by glyph, then "…" at the prefix's end
    // rounded up to a pixel (where `fit_line` measured it).
    const auto line = [&](const text::shaped_line& l, const text::line_fit& lf, std::int32_t pen) {
        const unsigned end = lf.glyph_end < l.count ? lf.glyph_end : l.count;
        for (unsigned i = 0; i < end; ++i) {
            ok = add_glyph(run, face, size, hinting, l.glyph[i], pen, l.x_off[i], l.y_off[i]) && ok;
            pen += l.x_adv[i];
        }
        if (lf.ellipsized) {
            pen = ((pen + 63) / 64) * 64;
            for (unsigned i = 0; i < ellipsis_->count; ++i) {
                ok = add_glyph(run, face, size, hinting, ellipsis_->glyph[i], pen,
                               ellipsis_->x_off[i], ellipsis_->y_off[i]) &&
                     ok;
                pen += ellipsis_->x_adv[i];
            }
        }
    };
    if (fit.width_px > 0)
        line(*line_, fit, 0);
    if (suffix_fit.width_px > 0)
        line(*suffix_, suffix_fit, fit.width_px * 64);
    const text::shaped_line& metrics = fit.width_px > 0 ? *line_ : *suffix_;
    run.scale = scale;
    run.width = text_w;
    run.ascent = metrics.ascent_px;
    run.descent = metrics.descent_px;
    run.glyph_end = fit.glyph_end;
    run.ellipsized = fit.ellipsized;
    run.not_responding = not_responding;
    if (!ok)
        return lx::invalid_argument("chrome_text: a title glyph could not be rasterised");
    return true;
}

// ── frame_node ─────────────────────────────────────────────────────────────

lx::compositor::frame_node::~frame_node() = default;

lx::scene::node_kind lx::compositor::frame_node::kind() const {
    // Not a container, so `scene_graph::emit_node` asks it for draws.
    return scene::node_kind::surface;
}

void lx::compositor::frame_node::emit_draws(scene::draw_list& out,
                                           const scene::emit_context& ctx) const {
    if (!shown || !atlas_ready || !*atlas_ready || geometry.width <= 0 || geometry.height <= 0)
        return;
    const lx::rect2i g = geometry;
    const lx::point2i anchor = ctx.anchored ? ctx.anchor : lx::point2i{g.x, g.y};
    const auto project = [&](lx::rect2i r) {
        if (r.width <= 0 || r.height <= 0)
            return lx::rect2i{};
        return scene::project_node(ctx.projection, anchor, {r.x, r.y}, {r.width, r.height});
    };
    const auto push_draw = [&](lx::scene::draw_list& list, unsigned texture, lx::rect2i src,
                               lx::rect2i dst, lx::rect2i clip, lx::blend_mode blend,
                               lx::color tint) {
        ::push_draw(list, texture, src, project(dst), project(clip), blend, tint);
    };
    const lx::rect2i f = frame_rect(g, extents);
    const lx::color tint = dimmed ? k_dim_tint : k_no_tint;
    const unsigned atlas = k_deco_atlas_texture_id;
    // The solid fills: one opaque quad per rect, ordered in the frame's paint order (the
    // first starts the list's primitive stream; the surfaces after it take their orders at
    // push, above it).
    const auto push_fill = [&](lx::rect2i logical, std::uint32_t rgb) {
        const lx::rect2i r = project(logical);
        if (r.width <= 0 || r.height <= 0)
            return;
        lx::gfx::quad_instance q{};
        q.bounds[0] = static_cast<float>(r.x);
        q.bounds[1] = static_cast<float>(r.y);
        q.bounds[2] = static_cast<float>(r.width);
        q.bounds[3] = static_cast<float>(r.height);
        const lx::color c = deco_fill_color(rgb, dimmed);
        const lx::gfx::packed_rgba bg = lx::gfx::premultiplied_half(c);
        q.background[0] = q.background_end[0] = bg.r;
        q.background[1] = q.background_end[1] = bg.g;
        q.background[2] = q.background_end[2] = bg.b;
        q.background[3] = q.background_end[3] = bg.a;
        out.push_quad(q, r, out.orders().assign(r));
    };
    const std::uint32_t border = active ? k_border_active : k_border_inactive;
    // Borders: left and right full height, top and bottom between them.
    if (extents.left > 0)
        push_fill({f.x, f.y, extents.left, f.height}, border);
    if (extents.right > 0)
        push_fill({g.x + g.width, f.y, extents.right, f.height}, border);
    if (extents.top > k_deco_title_height)
        push_fill({g.x, f.y, g.width, extents.top - k_deco_title_height}, border);
    if (extents.bottom > 0)
        push_fill({g.x, g.y + g.height, g.width, extents.bottom}, border);

    const lx::rect2i title = title_rect(g);
    push_fill(title, active ? k_title_active : k_title_inactive);
    const double vs = ctx.projection.scale > 0.0 ? ctx.projection.scale : 1.0;
    const lx::rect2i bar = project(title);

    // A glyph sprite over `box` (output pixels, fractional when resampled) from tile `id`,
    // clipped to `clip`, at `order`. False when the tile is gone (the atlas evicted it).
    const auto push_glyph = [&](gfx::atlas_tile_id id, bool color, const float box[4],
                                lx::rect2i clip, gfx::packed_rgba rgba, std::uint32_t order) {
        gfx::atlas_tile tile{};
        if (!text->atlas().atlas().use(id, tile))
            return false;
        const int x0 = static_cast<int>(std::floor(box[0]));
        const int y0 = static_cast<int>(std::floor(box[1]));
        const int x1 = static_cast<int>(std::ceil(box[0] + box[2]));
        const int y1 = static_cast<int>(std::ceil(box[1] + box[3]));
        const int vx0 = std::max(x0, clip.x);
        const int vy0 = std::max(y0, clip.y);
        const int vx1 = std::min(x1, clip.x + clip.width);
        const int vy1 = std::min(y1, clip.y + clip.height);
        if (vx1 <= vx0 || vy1 <= vy0)
            return true;
        gfx::sprite_instance sp{};
        for (unsigned i = 0; i < 4; ++i)
            sp.bounds[i] = box[i];
        sp.clip[0] = static_cast<float>(clip.x);
        sp.clip[1] = static_cast<float>(clip.y);
        sp.clip[2] = static_cast<float>(clip.width);
        sp.clip[3] = static_cast<float>(clip.height);
        const gfx::sprite_uv uv = tile.uv();
        sp.uv[0] = uv.u0;
        sp.uv[1] = uv.v0;
        sp.uv[2] = uv.u1;
        sp.uv[3] = uv.v1;
        sp.page = tile.page;
        sp.color[0] = rgba.r;
        sp.color[1] = rgba.g;
        sp.color[2] = rgba.b;
        sp.color[3] = rgba.a;
        if (!color) {
            sp.params[0] = 0x3C00u; // f16 1: the identity rotation (cos, sin, −sin, cos)
            sp.params[3] = 0x3C00u;
        }
        out.push_sprite(color ? gfx::primitive_kind::poly_sprite : gfx::primitive_kind::mono_sprite,
                        tile.page, sp, {vx0, vy0, vx1 - vx0, vy1 - vy0}, order);
        return true;
    };

    const title_run* run = this->title && text ? this->title->for_scale(vs) : nullptr;
    if (run && run->width > 0) {
        // Placed in output pixels: the run rasterised for this output's scale is drawn 1:1
        // (k = 1); a run of another scale is resampled. Centred in the room left of the close
        // button, within the projected bar, the line box centred vertically — where the CPU
        // title textures were.
        const double k = vs / run->scale;
        const int room = static_cast<int>(std::lround(title_text_room(g.width) * vs));
        const int text_w = static_cast<int>(std::lround(run->width * k));
        const int h = static_cast<int>(std::lround((run->ascent + run->descent) * k));
        const int x0 = bar.x + static_cast<int>(std::lround(k_deco_title_padding * vs));
        const int x = x0 + (room > text_w ? (room - text_w) / 2 : 0);
        const int y = bar.y + (bar.height - h) / 2;
        const int baseline = y + static_cast<int>(std::lround(run->ascent * k));
        const lx::rect2i clip =
            project({title.x, title.y, g.width - k_deco_title_height, k_deco_title_height});
        const lx::rect2i span{std::max(x, clip.x), y, std::min(text_w, clip.width), h};
        if (clip.width > 0 && span.width > 0 && span.height > 0) {
            // One order for the run: its glyphs do not overlap anything of the frame's above
            // them, and the batch stays one draw per page.
            const std::uint32_t order = out.orders().assign(span);
            const gfx::packed_rgba fg = gfx::premultiplied_half(
                deco_fill_color(active ? k_glyph_light : k_glyph_dim, dimmed));
            const gfx::packed_rgba white = gfx::premultiplied_half(
                deco_fill_color(0xFFFFFFu, dimmed));
            bool stale = false;
            for (const title_glyph& gl : run->glyphs) {
                float box[4];
                if (k == 1.0) {
                    box[0] = static_cast<float>(x + gl.x);
                    box[1] = static_cast<float>(baseline + gl.y);
                    box[2] = static_cast<float>(gl.width);
                    box[3] = static_cast<float>(gl.height);
                } else {
                    box[0] = static_cast<float>(x + gl.x * k);
                    box[1] = static_cast<float>(baseline + gl.y * k);
                    box[2] = static_cast<float>(gl.width * k);
                    box[3] = static_cast<float>(gl.height * k);
                }
                if (!push_glyph(gl.tile, gl.color, box, clip, gl.color ? white : fg, order))
                    stale = true;
            }
            if (stale)
                text->note_stale();
        }
    }

    const lx::rect2i close = close_rect(g);
    if (button != deco_button::normal) {
        const lx::rect2i c =
            button == deco_button::pressed ? deco_atlas::circle_pressed : deco_atlas::circle_hover;
        push_draw(out, atlas, c,
                  {close.x + (close.width - c.width) / 2, close.y + (close.height - c.height) / 2,
                   c.width, c.height},
                  title, lx::blend_mode::premultiplied, tint);
    }
    const bool light = button != deco_button::normal || active;
    if (const close_glyph* cg = text ? text->find_close(vs) : nullptr) {
        // The glyph rasterised for this scale, centred in the projected button.
        const lx::rect2i cr = project(close);
        const float box[4] = {static_cast<float>(cr.x + (cr.width - cg->size) / 2),
                              static_cast<float>(cr.y + (cr.height - cg->size) / 2),
                              static_cast<float>(cg->size), static_cast<float>(cg->size)};
        const lx::rect2i at{static_cast<int>(box[0]), static_cast<int>(box[1]), cg->size, cg->size};
        if (!push_glyph(cg->tile, false, box, bar,
                        gfx::premultiplied_half(
                            deco_fill_color(light ? k_glyph_light : k_glyph_dim, dimmed)),
                        out.orders().assign(at)))
            text->note_stale();
        return;
    }
    // Not rasterised for this scale (yet: the manager does at its next tick), or no text
    // atlas: the decoration atlas's glyph.
    const lx::rect2i glyph = light ? deco_atlas::close_light : deco_atlas::close_dim;
    push_draw(out, atlas, glyph,
              {close.x + (close.width - glyph.width) / 2,
               close.y + (close.height - glyph.height) / 2, glyph.width, glyph.height},
              title, lx::blend_mode::premultiplied, tint);
}

// ── decoration_manager ─────────────────────────────────────────────────────

void lx::compositor::decoration_manager::bind(surface_tree& tree, scene::scene_graph& scene) {
    tree_ = &tree;
    scene_ = &scene;
}

lx::compositor::decoration_manager::slot*
lx::compositor::decoration_manager::alloc(const toplevel_record& rec) {
    // By index: iteration skips blocks never allocated, where the free slots may be.
    for (unsigned i = 0; i < slots_.size(); ++i) {
        slot& s = slots_[i];
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
        s.not_responding = false;
        s.scales[0] = scale_;
        s.scale_count = 1;
        for (title_run& r : s.text.runs) {
            r.scale = 0.0;
            r.width = 0;
            r.glyphs.clear();
        }
        s.node.dimmed = false;
        s.node.geometry = {};
        s.node.extents = {};
        s.node.shown = false;
        s.node.active = rec.id == active_;
        s.node.button = deco_button::normal;
        s.node.title = &s.text;
        s.node.text = text_.get();
        s.node.atlas_ready = &atlas_ready_;
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
    // No frame around a fullscreen window (M11).
    const bool want = rec.mapped && rec.decoration_current == decoration_mode::server &&
                      rec.state != lx::toplevel_state::fullscreen;
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
                log_deco.warn("decoration table full — frame not drawn. "
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
    if (s->not_responding != rec.not_responding)
        set_not_responding(rec.id, rec.not_responding);
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
    if ((old || s) && log_deco.enabled(lx::trace::level::debug)) {
        log_deco.debug("decoration active id={} previous={}", id.id(), previous.id());
    }
}

void lx::compositor::decoration_manager::set_not_responding(lx::toplevel_id id, bool on) {
    slot* s = find(id);
    if (!s || s->not_responding == on)
        return;
    s->not_responding = on;
    s->node.dimmed = on;
    s->title_changed = true;
    any_dirty_ = true;
    damage_strips(s->node);
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
    // Its glyphs stay in the atlas, shared with every title that uses them, until they age out.
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

void lx::compositor::decoration_manager::bind_chrome(chrome_atlas* atlas) {
    text_ = atlas ? std::make_unique<chrome_text>(*atlas) : nullptr;
    for (auto& s : slots_) {
        s.node.text = text_.get();
        s.title_changed = true;
    }
    any_dirty_ = true;
}

void lx::compositor::decoration_manager::set_scale(double scale) {
    if (!(scale > 0.0) || scale == scale_)
        return;
    scale_ = scale;
}

void lx::compositor::decoration_manager::set_window_scale(lx::toplevel_id id, double scale) {
    const double one[1] = {scale};
    set_window_scales(id, one);
}

void lx::compositor::decoration_manager::set_window_scales(lx::toplevel_id id,
                                                           std::span<const double> scales) {
    slot* s = find(id);
    if (!s)
        return;
    double want[k_title_scales]{};
    unsigned n = 0;
    for (const double v : scales) {
        if (!(v > 0.0) || n == k_title_scales)
            continue;
        bool seen = false;
        for (unsigned i = 0; i < n; ++i)
            seen = seen || want[i] == v;
        if (!seen)
            want[n++] = v;
    }
    if (n == 0)
        return;
    bool same = n == s->scale_count;
    for (unsigned i = 0; i < n && same; ++i) {
        bool found = false;
        for (unsigned k = 0; k < s->scale_count; ++k)
            found = found || s->scales[k] == want[i];
        same = found;
    }
    if (same)
        return;
    for (unsigned i = 0; i < k_title_scales; ++i)
        s->scales[i] = i < n ? want[i] : 0.0;
    s->scale_count = n;
    // Runs of the scales kept stay as they are; the new ones are laid out at the next tick.
    s->refit = true;
    any_dirty_ = true;
}

void lx::compositor::decoration_manager::raster_title(slot& s) {
    const bool changed = s.title_changed;
    s.title_changed = false;
    s.refit = false;
    frame_node& n = s.node;
    const auto warn_no_text = [&] {
        if (!warned_no_text_) {
            warned_no_text_ = true;
            log_deco.warn("no title text: lx.text cannot shape (no font "
                          "face, or built without HarfBuzz). Reported once.");
        }
    };
    if (!text_) {
        warn_no_text();
        return;
    }
    // Runs of scales the window left are dropped (their storage kept for the next).
    bool damaged = false;
    for (title_run& r : s.text.runs) {
        bool wanted = false;
        for (unsigned i = 0; i < s.scale_count; ++i)
            wanted = wanted || r.scale == s.scales[i];
        if (r.scale > 0.0 && !wanted) {
            damaged = damaged || r.width > 0;
            r.scale = 0.0;
            r.width = 0;
            r.glyphs.clear();
        }
    }
    const int room = title_text_room(n.geometry.width);
    for (unsigned i = 0; i < s.scale_count; ++i) {
        const double scale = s.scales[i];
        // The close glyph at this scale (shared by every frame; rasterised once per scale).
        if (!text_->find_close(scale) || changed) {
            if (text_->close(scale) && scene_ && n.shown)
                scene_->note_damage(close_rect(n.geometry));
        }
        title_run* run = nullptr;
        for (title_run& r : s.text.runs)
            if (r.scale == scale)
                run = &r;
        bool fresh = false;
        if (!run) {
            for (title_run& r : s.text.runs)
                if (!run && !(r.scale > 0.0))
                    run = &r;
            fresh = true;
        }
        if (!run)
            break;
        auto laid = text_->layout_title(*run, s.title, s.not_responding, room, scale,
                                        changed || fresh);
        if (!laid) {
            // What could be laid out is drawn; the rest is tried again at the next change.
            if (!warned_no_text_) {
                warned_no_text_ = true;
                log_deco.error(laid.get_error());
            }
            damaged = true;
            continue;
        }
        damaged = damaged || laid.value();
        if (laid.value() && log_deco.enabled(lx::trace::level::debug)) {
            log_deco.debug(
                "decoration title id={} scale={} width={} ellipsized={} title=\"{:.200}{}\"",
                s.id.id(), scale, run->width, run->ellipsized ? 1 : 0, s.title,
                s.not_responding ? " (Not Responding)" : "");
        }
    }
    if (damaged)
        damage_title(n);
}

void lx::compositor::decoration_manager::on_tick() {
    if (text_) {
        // The atlas's frame; tiles that moved (a page rebuild) are drawn where they now are by
        // the next emission, which a full damage of the frames makes happen.
        if (text_->atlas().begin_frame()) {
            for (const auto& s : slots_)
                if (s.used)
                    damage_strips(s.node);
        }
        // A tile an emission found evicted: every title is laid out (and rasterised) again.
        if (text_->take_stale()) {
            for (auto& s : slots_)
                if (s.used) {
                    s.title_changed = true;
                    damage_title(s.node);
                }
            any_dirty_ = true;
        }
    }
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
