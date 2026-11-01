// Server-side frame geometry (lx.compositor:decoration): extents, frame rect, hit zones,
// the close glyph's coverage, frame-aware placement, and what a frame draws: primitives only
// (fills and the button's circle as quads, the × as a mono sprite), from the first frame it
// is shown, the × at a scale the moment the manager learns it, and on a GLES 2 context (no
// primitive renderer: the lowered blits) the circle and the × still there.
#include <fcntl.h>
#include <unistd.h>

#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.compositor;

using namespace lx::compositor;

namespace {

constexpr lx::rect2i k_geom{100, 200, 400, 300}; // client window geometry, global
constexpr int T = k_deco_title_height;
constexpr int R = k_deco_resize_margin;
constexpr int K = k_deco_corner;

deco_zone hit(double x, double y, bool maximized = false) {
    return deco_hit(k_geom, extents_for(decoration_mode::server, maximized), x, y);
}

} // namespace

LUMEN_TEST(extents_per_mode) {
    const frame_extents client = extents_for(decoration_mode::client, false);
    LUMEN_CHECK(client.empty());
    LUMEN_CHECK(extents_for(decoration_mode::client, true).empty());
    const frame_extents server = extents_for(decoration_mode::server, false);
    LUMEN_CHECK(server.left == 1 && server.top == T + 1 && server.right == 1 && server.bottom == 1);
    const frame_extents maxed = extents_for(decoration_mode::server, true);
    LUMEN_CHECK(maxed.left == 0 && maxed.top == T && maxed.right == 0 && maxed.bottom == 0);
}

LUMEN_TEST(frame_rect_grows_by_the_extents) {
    const lx::rect2i f = frame_rect(k_geom, extents_for(decoration_mode::server, false));
    LUMEN_CHECK(f.x == 99 && f.y == 200 - T - 1 && f.width == 402 && f.height == 300 + T + 2);
    const lx::rect2i same = frame_rect(k_geom, extents_for(decoration_mode::client, false));
    LUMEN_CHECK(same.x == k_geom.x && same.y == k_geom.y && same.width == k_geom.width &&
                same.height == k_geom.height);
}

LUMEN_TEST(close_wins_over_title_and_title_spans_the_bar) {
    const int ty = k_geom.y - T; // title bar top
    // Close button: the bar's right T x T.
    LUMEN_CHECK(hit(k_geom.x + k_geom.width - 1, ty) == deco_zone::close);
    LUMEN_CHECK(hit(k_geom.x + k_geom.width - T, ty + T - 1) == deco_zone::close);
    LUMEN_CHECK(hit(k_geom.x + k_geom.width - T - 1, ty + 5) == deco_zone::title);
    LUMEN_CHECK(hit(k_geom.x, ty) == deco_zone::title);
    LUMEN_CHECK(hit(k_geom.x + 200, k_geom.y - 1) == deco_zone::title);
    // The client area is nobody's (its surfaces are hit first).
    LUMEN_CHECK(hit(k_geom.x, k_geom.y) == deco_zone::none);
    LUMEN_CHECK(hit(k_geom.x + 200, k_geom.y + 150) == deco_zone::none);
}

LUMEN_TEST(resize_bands_reach_the_invisible_margin_and_no_further) {
    const lx::rect2i f = frame_rect(k_geom, extents_for(decoration_mode::server, false));
    const double mid_y = k_geom.y + k_geom.height / 2.0;
    const double mid_x = k_geom.x + k_geom.width / 2.0;
    // Right: the visible border pixel, the margin, and just outside it.
    LUMEN_CHECK(hit(k_geom.x + k_geom.width, mid_y) == deco_zone::right);
    LUMEN_CHECK(hit(f.x + f.width + R - 1, mid_y) == deco_zone::right);
    LUMEN_CHECK(hit(f.x + f.width + R, mid_y) == deco_zone::none);
    // Left.
    LUMEN_CHECK(hit(k_geom.x - 1, mid_y) == deco_zone::left);
    LUMEN_CHECK(hit(f.x - R, mid_y) == deco_zone::left);
    LUMEN_CHECK(hit(f.x - R - 1, mid_y) == deco_zone::none);
    // Bottom.
    LUMEN_CHECK(hit(mid_x, k_geom.y + k_geom.height) == deco_zone::bottom);
    LUMEN_CHECK(hit(mid_x, f.y + f.height + R - 1) == deco_zone::bottom);
    LUMEN_CHECK(hit(mid_x, f.y + f.height + R) == deco_zone::none);
    // Top: above the title bar — the border pixel and the margin.
    LUMEN_CHECK(hit(mid_x, f.y) == deco_zone::top);
    LUMEN_CHECK(hit(mid_x, f.y - R) == deco_zone::top);
    LUMEN_CHECK(hit(mid_x, f.y - R - 1) == deco_zone::none);
}

LUMEN_TEST(corners_take_precedence_within_the_corner_length) {
    const lx::rect2i f = frame_rect(k_geom, extents_for(decoration_mode::server, false));
    // Along the top band.
    LUMEN_CHECK(hit(f.x + K - 1, f.y - 2) == deco_zone::top_left);
    LUMEN_CHECK(hit(f.x + K, f.y - 2) == deco_zone::top);
    LUMEN_CHECK(hit(f.x + f.width - K, f.y - 2) == deco_zone::top_right);
    LUMEN_CHECK(hit(f.x + f.width - K - 1, f.y - 2) == deco_zone::top);
    // Along the left band, beside the title bar: still the corner near the top.
    LUMEN_CHECK(hit(f.x - 3, f.y + K - 1) == deco_zone::top_left);
    LUMEN_CHECK(hit(f.x - 3, f.y + K) == deco_zone::left);
    // Bottom corners along both bands.
    LUMEN_CHECK(hit(f.x - 3, f.y + f.height - K) == deco_zone::bottom_left);
    LUMEN_CHECK(hit(f.x + f.width - K, f.y + f.height + 2) == deco_zone::bottom_right);
    LUMEN_CHECK(hit(f.x + f.width + 2, f.y + f.height - 1) == deco_zone::bottom_right);
    // The outer corner of the margin itself.
    LUMEN_CHECK(hit(f.x - R, f.y - R) == deco_zone::top_left);
    LUMEN_CHECK(hit(f.x + f.width + R - 1, f.y + f.height + R - 1) == deco_zone::bottom_right);
}

LUMEN_TEST(maximized_has_a_title_bar_and_no_resize_bands) {
    LUMEN_CHECK(hit(k_geom.x + 5, k_geom.y - 5, true) == deco_zone::title);
    LUMEN_CHECK(hit(k_geom.x + k_geom.width - 3, k_geom.y - 3, true) == deco_zone::close);
    LUMEN_CHECK(hit(k_geom.x - 1, k_geom.y + 10, true) == deco_zone::none);
    LUMEN_CHECK(hit(k_geom.x + 10, k_geom.y + k_geom.height, true) == deco_zone::none);
    LUMEN_CHECK(hit(k_geom.x + 10, k_geom.y - T - 1, true) == deco_zone::none);
}

LUMEN_TEST(client_side_has_no_zones) {
    const frame_extents none = extents_for(decoration_mode::client, false);
    LUMEN_CHECK(deco_hit(k_geom, none, k_geom.x - 2, k_geom.y + 10) == deco_zone::none);
    LUMEN_CHECK(deco_hit(k_geom, none, k_geom.x + 10, k_geom.y - 5) == deco_zone::none);
}

LUMEN_TEST(zone_edges_and_cursors) {
    LUMEN_CHECK(edges_for_zone(deco_zone::title) == edge_none);
    LUMEN_CHECK(edges_for_zone(deco_zone::close) == edge_none);
    LUMEN_CHECK(edges_for_zone(deco_zone::top_left) == (edge_top | edge_left));
    LUMEN_CHECK(edges_for_zone(deco_zone::bottom_right) == (edge_bottom | edge_right));
    LUMEN_CHECK(edges_for_zone(deco_zone::right) == edge_right);
    for (unsigned z = 0; z <= static_cast<unsigned>(deco_zone::bottom_right); ++z)
        LUMEN_CHECK(valid_resize_edges(edges_for_zone(static_cast<deco_zone>(z))));
    LUMEN_CHECK(cursor_for_zone(deco_zone::title) == cursor_shape::default_);
    LUMEN_CHECK(cursor_for_zone(deco_zone::left) == shape_for_edges(edge_left));
    LUMEN_CHECK(cursor_for_zone(deco_zone::top_right) == shape_for_edges(edge_top | edge_right));
}

LUMEN_TEST(title_room_and_rects) {
    LUMEN_CHECK(title_text_room(400) == 400 - 2 * k_deco_title_padding - T);
    const lx::rect2i t = title_rect(k_geom);
    LUMEN_CHECK(t.x == k_geom.x && t.y == k_geom.y - T && t.width == k_geom.width && t.height == T);
    const lx::rect2i c = close_rect(k_geom);
    LUMEN_CHECK(c.x + c.width == k_geom.x + k_geom.width && c.y == t.y && c.width == T);
}

LUMEN_TEST(close_glyph_coverage_is_an_antialiased_cross) {
    for (const double scale : {1.0, 2.0}) {
        const int n = static_cast<int>(std::lround(k_deco_close_glyph * scale));
        std::vector<std::uint8_t> cov(static_cast<std::size_t>(n * n));
        build_close_coverage(scale, n, cov.data());
        const auto at = [&](int x, int y) { return cov[static_cast<std::size_t>(y * n + x)]; };
        // Opaque where the strokes cross, clear in the middle of each gap, symmetric.
        LUMEN_CHECK(at(n / 2, n / 2) > 200);
        LUMEN_CHECK(at(n / 2, 0) < 40 && at(0, n / 2) < 40);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                LUMEN_CHECK(at(x, y) == at(y, x) && at(x, y) == at(n - 1 - x, y));
    }
}

LUMEN_TEST(placement_with_extents_keeps_the_title_bar_on_the_output) {
    const lx::rect2i output{0, 0, 1920, 1080};
    const frame_extents ext = extents_for(decoration_mode::server, false);
    // A window the size of the output: its frame is taller than the output, and the frame's
    // top (the title bar) is what must stay on it.
    const lx::rect2i big{0, 0, 1920, 1080};
    const lx::rect2i f0 = frame_rect(big, ext);
    const lx::point2i p = place_new_window(output, {f0.width, f0.height}, nullptr, 0);
    const lx::point2i c = constrain_on_output(output, {p.x, p.y, f0.width, f0.height});
    LUMEN_CHECK(c.y >= output.y);
    // Dragged far up: the constrained frame top stays at the output's top, so the client
    // geometry sits a title bar plus border below it.
    const lx::rect2i dragged = frame_rect({300, -500, 400, 300}, ext);
    const lx::point2i k = constrain_on_output(output, dragged);
    LUMEN_CHECK(k.y == output.y);
    LUMEN_CHECK(k.y + ext.top == T + 1);
}

namespace {

/// The compositor's text atlas with the close glyph rasterised at the scales these tests use
/// (no font is needed for it).
struct chrome {
    chrome_atlas atlas{};
    chrome_text text{atlas};
    chrome() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        for (const double s : {1.0, 1.25})
            LUMEN_CHECK(text.close(s) != nullptr);
    }
};
chrome& chrome_env() {
    static chrome c{};
    return c;
}

/// A frame node's emission composited on the CPU backend into a black XRGB target, the way the
/// render thread does: the sorted stream through `ui_cpu_renderer`, sprites from the text
/// atlas, the compositor drawing the `surface` refs.
struct composited {
    std::vector<std::uint32_t> px;
    int w = 0, h = 0;
    [[nodiscard]] std::uint32_t at(int x, int y) const {
        return px[static_cast<std::size_t>(y * w + x)] & 0x00FFFFFFu;
    }
};

/// The frame palette as Phase 1's 4x4 swatches (16x4 premultiplied ARGB, one swatch per fill
/// colour), registered under a test id: what `frame_quads_draw_the_pixels_the_swatch_blits_drew`
/// compares the quads against.
constexpr unsigned k_swatch_texture = 0x7FFF'FE00u;
constexpr std::uint32_t k_swatches[4] = {k_deco_title_active, k_deco_title_inactive,
                                         k_deco_border_active, k_deco_border_inactive};

lx::gfx::cpu_compositor& deco_compositor() {
    static std::vector<std::uint32_t> swatches(16 * 4);
    static lx::gfx::cpu_compositor comp;
    static bool registered = false;
    if (!registered) {
        registered = true;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 16; ++x)
                swatches[static_cast<std::size_t>(y * 16 + x)] = 0xFF000000u | k_swatches[x / 4];
        lx::gfx::pixel_surface tex{};
        tex.pixels = reinterpret_cast<unsigned char*>(swatches.data());
        tex.width = 16;
        tex.height = 4;
        tex.stride = 16 * 4u;
        tex.format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
        (void)comp.register_texture(k_swatch_texture, tex);
    }
    return comp;
}

lx::gfx::blit_command blit_of(const lx::scene::draw_command& d) {
    lx::gfx::blit_command b{};
    b.texture_id = d.texture.id();
    b.dst = d.dst;
    b.src = d.src;
    b.clip = d.clip;
    b.tint = d.tint;
    b.opacity = d.opacity;
    b.blend = d.blend;
    b.scaled = d.scaled;
    b.solid = d.solid;
    b.solid_color = d.solid_color;
    return b;
}

composited blank(int w, int h) {
    return {std::vector<std::uint32_t>(static_cast<std::size_t>(w * h), 0u), w, h};
}

lx::gfx::pixel_surface surface_of(composited& c) {
    lx::gfx::pixel_surface dst{};
    dst.pixels = reinterpret_cast<unsigned char*>(c.px.data());
    dst.width = static_cast<unsigned>(c.w);
    dst.height = static_cast<unsigned>(c.h);
    dst.stride = static_cast<unsigned>(c.w) * 4u;
    dst.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    return dst;
}

composited composite_frame(const frame_node& node, int w, int h,
                           const lx::scene::emit_context& ctx = {}) {
    lx::gfx::cpu_compositor& comp = deco_compositor();
    lx::scene::draw_list list;
    node.emit_draws(list, ctx);
    list.batch_for_render();
    std::vector<lx::gfx::blit_command> blits(list.size());
    for (unsigned i = 0; i < list.size(); ++i)
        blits[i] = blit_of(list.data()[i]);
    composited out = blank(w, h);
    static lx::gfx::ui_cpu_renderer ui;
    ui.set_surface_compositor(&comp);
    lx::gfx::ui_stream st{};
    st.refs = list.refs();
    st.batches = list.batches();
    st.quads = list.quad_instances();
    st.sprites = list.sprite_instances();
    st.surfaces = blits;
    lx::gfx::damage_region full{};
    full.full = true;
    const std::lock_guard<std::mutex> lock{chrome_env().atlas.mutex()};
    (void)ui.draw(surface_of(out), st, &chrome_env().atlas.atlas(), full, lx::color::rgb(0.f, 0.f, 0.f));
    return out;
}

/// What a frame without glyphs drew before its fills became quads: each fill a blit of its
/// palette swatch's centre (opaque, tinted by the dim), through `cpu_compositor::composite` —
/// the old render path, rebuilt here.
composited composite_frame_before(const frame_node& node, int w, int h,
                                  const lx::scene::emit_context& ctx = {}) {
    lx::gfx::cpu_compositor& comp = deco_compositor();
    const lx::rect2i g = node.geometry;
    const lx::point2i anchor = ctx.anchored ? ctx.anchor : lx::point2i{g.x, g.y};
    const auto project = [&](lx::rect2i r) {
        return lx::scene::project_node(ctx.projection, anchor, {r.x, r.y}, {r.width, r.height});
    };
    const lx::color tint = node.dimmed ? lx::color::rgb(0.65f, 0.65f, 0.65f, 1.f)
                                       : lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    std::vector<lx::gfx::blit_command> blits;
    const auto fill = [&](lx::rect2i logical, std::uint32_t rgb) {
        if (logical.width <= 0 || logical.height <= 0)
            return;
        unsigned swatch = 0;
        while (k_swatches[swatch] != rgb)
            ++swatch;
        lx::gfx::blit_command b{};
        b.texture_id = k_swatch_texture;
        b.src = {static_cast<float>(swatch * 4 + 1), 1.f, 2.f, 2.f}; // the swatch's centre
        b.dst = project(logical);
        b.tint = tint;
        b.blend = lx::blend_mode::opaque;
        blits.push_back(b);
    };
    const frame_extents e = node.extents;
    const lx::rect2i f = frame_rect(g, e);
    const std::uint32_t border = node.active ? k_deco_border_active : k_deco_border_inactive;
    if (e.left > 0)
        fill({f.x, f.y, e.left, f.height}, border);
    if (e.right > 0)
        fill({g.x + g.width, f.y, e.right, f.height}, border);
    if (e.top > T)
        fill({g.x, f.y, g.width, e.top - T}, border);
    if (e.bottom > 0)
        fill({g.x, g.y + g.height, g.width, e.bottom}, border);
    fill(title_rect(g), node.active ? k_deco_title_active : k_deco_title_inactive);
    composited out = blank(w, h);
    (void)comp.composite(surface_of(out), lx::color::rgb(0.f, 0.f, 0.f), blits.data(),
                         static_cast<unsigned>(blits.size()), {});
    return out;
}

} // namespace

LUMEN_TEST(frame_draws_baked_colours_where_the_zones_are) {
    frame_node node{};
    node.geometry = {20, 40, 100, 50};
    node.extents = extents_for(decoration_mode::server, false);
    node.shown = true;
    node.active = true;
    node.text = &chrome_env().text;
    lx::scene::draw_list list;
    node.emit_draws(list);
    // Primitives only: four borders and the title fill as quads, the close glyph a mono
    // sprite (no title laid out, no hover); no surface draw, no texture.
    LUMEN_CHECK(list.primitive_count() == 6 && list.quad_instances().size() == 5);
    LUMEN_CHECK(list.sprite_instances().size() == 1);
    LUMEN_CHECK(list.size() == 0);

    const composited a = composite_frame(node, 160, 120);
    const lx::rect2i g = node.geometry;
    LUMEN_CHECK(a.at(g.x + 30, g.y - 14) == k_deco_title_active);
    LUMEN_CHECK(a.at(g.x + 30, g.y - T - 1) == k_deco_border_active);   // top border
    LUMEN_CHECK(a.at(g.x - 1, g.y + 20) == k_deco_border_active);       // left border
    LUMEN_CHECK(a.at(g.x + g.width, g.y + 20) == k_deco_border_active); // right border
    LUMEN_CHECK(a.at(g.x + 30, g.y + g.height) == k_deco_border_active); // bottom border
    LUMEN_CHECK(a.at(g.x + 30, g.y + 20) == 0u);                         // client area: untouched
    LUMEN_CHECK(a.at(g.x - 2, g.y + 20) == 0u);                          // outside: untouched
    // The close glyph's centre is light; the button's corner is the title colour.
    const lx::rect2i c = close_rect(g);
    const std::uint32_t mid = a.at(c.x + c.width / 2, c.y + c.height / 2);
    LUMEN_CHECK(((mid >> 16) & 0xFFu) > 0xC0u);
    LUMEN_CHECK(a.at(c.x + 1, c.y + 1) == k_deco_title_active);

    node.active = false;
    const composited i = composite_frame(node, 160, 120);
    LUMEN_CHECK(i.at(g.x + 30, g.y - 14) == k_deco_title_inactive);
    LUMEN_CHECK(i.at(g.x - 1, g.y + 20) == k_deco_border_inactive);

    node.active = true;
    node.button = deco_button::hover;
    lx::scene::draw_list hovered;
    node.emit_draws(hovered);
    // The circle: one more quad, its radius half its side.
    LUMEN_CHECK(hovered.quad_instances().size() == 6 && hovered.size() == 0);
    const lx::gfx::quad_instance& circle = hovered.quad_instances()[5];
    LUMEN_CHECK(circle.bounds[2] == k_deco_close_circle && circle.bounds[3] == k_deco_close_circle);
    for (const std::uint16_t r : circle.radii)
        LUMEN_CHECK(lx::gfx::from_half(r) == k_deco_close_circle / 2.f);
    const composited h = composite_frame(node, 160, 120);
    // Inside the circle, off the glyph's strokes: the hover red; the button's corner (outside
    // the circle) the title colour.
    LUMEN_CHECK(h.at(c.x + 6, c.y + c.height / 2) == k_deco_button_hover);
    LUMEN_CHECK(h.at(c.x + 1, c.y + 1) == k_deco_title_active);
    node.button = deco_button::pressed;
    const composited p = composite_frame(node, 160, 120);
    LUMEN_CHECK(p.at(c.x + 6, c.y + c.height / 2) == k_deco_button_pressed);

    // Maximized: no borders, the title bar directly above the client.
    node.button = deco_button::normal;
    node.extents = extents_for(decoration_mode::server, true);
    const composited m = composite_frame(node, 160, 120);
    LUMEN_CHECK(m.at(g.x - 1, g.y + 20) == 0u);
    LUMEN_CHECK(m.at(g.x + 30, g.y - 1) == k_deco_title_active);
    LUMEN_CHECK(m.at(g.x + 30, g.y - T - 1) == 0u);
}

LUMEN_TEST(frame_quads_draw_the_pixels_the_swatch_blits_drew) {
    // ADR-010 §4.11: the fills moved from atlas swatch blits to quad primitives with the
    // same rects and colours; on the CPU compositor at scale 1 the frame is pixel-identical
    // in every state (and at a fractional scale, where the rects round the same way). The
    // circle and the glyphs are measured in test_deco_titles.
    frame_node node{};
    node.geometry = {20, 40, 100, 50};
    node.shown = true;
    lx::scene::emit_context scaled{};
    scaled.projection.scale = 1.25;
    const lx::scene::emit_context* const contexts[2] = {nullptr, &scaled};
    unsigned compared = 0;
    for (const bool maximized : {false, true})
        for (const bool active : {true, false})
            for (const bool dimmed : {false, true})
                for (const lx::scene::emit_context* ctx : contexts) {
                    node.extents = extents_for(decoration_mode::server, maximized);
                    node.active = active;
                    node.dimmed = dimmed;
                    const lx::scene::emit_context c = ctx ? *ctx : lx::scene::emit_context{};
                    const composited before = composite_frame_before(node, 200, 150, c);
                    const composited after = composite_frame(node, 200, 150, c);
                    unsigned diffs = 0;
                    for (int y = 0; y < 150; ++y)
                        for (int x = 0; x < 200; ++x)
                            diffs += before.at(x, y) != after.at(x, y) ? 1u : 0u;
                    if (diffs != 0)
                        std::printf("    max=%d active=%d dimmed=%d scaled=%d: %u pixel(s) differ\n",
                                    maximized, active, dimmed, ctx != nullptr, diffs);
                    LUMEN_CHECK(diffs == 0);
                    ++compared;
                }
    LUMEN_CHECK(compared == 16);
    // The dimmed fill is the CPU compositor's tinted byte: (0x30 · 166) >> 8 = 0x1F.
    node.extents = extents_for(decoration_mode::server, false);
    node.active = true;
    node.dimmed = true;
    const composited d = composite_frame(node, 200, 150);
    LUMEN_CHECK(d.at(node.geometry.x + 30, node.geometry.y - 14) == 0x1F1F21u);
}

// Phase 5: no atlas to wait for — a frame draws from its first frame; hidden, nothing.
LUMEN_TEST(frame_draws_from_its_first_frame_and_nothing_when_hidden) {
    frame_node node{};
    node.geometry = {20, 40, 100, 50};
    node.extents = extents_for(decoration_mode::server, false);
    node.shown = true;
    lx::scene::draw_list list;
    node.emit_draws(list);
    LUMEN_CHECK(list.quad_instances().size() == 5 && list.size() == 0);
    lx::scene::draw_list hidden;
    node.shown = false;
    node.emit_draws(hidden);
    LUMEN_CHECK(hidden.size() == 0 && hidden.primitive_count() == 0);
}

namespace {

/// A decorated toplevel the manager frames.
lx::compositor::toplevel_record decorated_record() {
    lx::compositor::toplevel_record rec{};
    rec.id = lx::toplevel_id{7};
    rec.surface = lx::surface_id{70};
    rec.mapped = true;
    rec.decoration_current = decoration_mode::server;
    rec.geometry = {40, 60, 300, 200};
    return rec;
}

/// The frame published in view 0 of `scene` at `scale`: its quads and mono sprites.
struct published {
    unsigned quads = 0;
    unsigned sprites = 0;
    unsigned surfaces = 0;
    /// The largest sprite's edge (the × at the view's scale is round(10 × scale)).
    int sprite_edge = 0;
};

published publish(lx::scene::scene_graph& scene, double scale, unsigned frame) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::output_view_config v{};
    v.logical = {0, 0, static_cast<int>(800 / scale), static_cast<int>(600 / scale)};
    v.projection = {{0, 0}, scale, {800, 600}};
    scene.set_output_view(0, v);
    scene.note_full_damage_output(0);
    LUMEN_CHECK(scene.commit_frame(0, frame));
    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = scene.acquire_render_snapshot(0);
    published p{};
    p.quads = static_cast<unsigned>(snap.draws().quad_instances().size());
    p.sprites = static_cast<unsigned>(snap.draws().sprite_instances().size());
    p.surfaces = snap.draws().size();
    for (const lx::gfx::sprite_instance& sp : snap.draws().sprite_instances())
        p.sprite_edge = std::max(p.sprite_edge, static_cast<int>(sp.bounds[2]));
    scene.release_render_frame(0, snap);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return p;
}

} // namespace

// The first snapshot published after a frame is shown carries its border and title fills and
// its × — no tick in between (the manager's tick only lays titles out), no atlas staged first.
// And when the window reaches an output of a scale not seen before, the × at that scale is
// rasterised as the manager learns the scale: the next frame published draws it.
LUMEN_TEST(managed_frame_draws_its_fills_and_close_from_the_first_snapshot) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    chrome_atlas atlas{};
    decoration_manager m{};
    m.bind_chrome(&atlas);
    lx::compositor::toplevel_record rec = decorated_record();
    m.sync(rec); // shown: no `on_tick` before the publish
    const frame_node* n = m.node(rec.id);
    LUMEN_CHECK(n && n->shown);
    lx::scene::scene_graph scene;
    (void)scene.root().add(const_cast<frame_node*>(n));
    const published first = publish(scene, 1.0, 1);
    LUMEN_CHECK(first.quads == 5 && first.sprites == 1 && first.surfaces == 0);
    LUMEN_CHECK(first.sprite_edge == k_deco_close_glyph);
    // On an output of scale 2 now: the × at 2 is there in the next snapshot, still no tick.
    LUMEN_CHECK(m.text()->find_close(2.0) == nullptr);
    const double two[1] = {2.0};
    m.set_window_scales(rec.id, two);
    LUMEN_CHECK(m.text()->find_close(2.0) != nullptr);
    const published moved = publish(scene, 2.0, 2);
    LUMEN_CHECK(moved.quads == 5 && moved.sprites == 1 && moved.sprite_edge == 2 * k_deco_close_glyph);
    (void)scene.root().remove(const_cast<frame_node*>(n));
    m.on_destroyed(rec.id, rec.surface);
}

// A GLES 2 context (no `:ui_gl`): the frame's stream is lowered to blits, and the hover
// circle (a rounded solid blit) and the × (a coverage blit from the atlas's page) still draw,
// with nothing dropped.
LUMEN_TEST(gles2_context_draws_the_circle_and_the_close_glyph) {
    const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::printf("    (gl skip: no render node)\n");
        return;
    }
    {
        auto device = lx::gfx::egl_device::create(fd, true);
        if (!device || device->is_software_renderer()) {
            std::printf("    (gl skip: %s)\n", device ? "software renderer" : device.get_error().message);
            ::close(fd);
            return;
        }
        std::printf("    gles2: %s, context GLES %d.%d\n", device->renderer(), device->gles_major(),
                    device->gles_minor());
        lx::gfx::gl_compositor comp;
        LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));
        constexpr int W = 200, H = 120;
        auto target = lx::gfx::gl_scanout_target::create(*device, W, H,
                                                         static_cast<lx::fourcc>(lx::pixel_format::xrgb8888));
        LUMEN_CHECK(static_cast<bool>(target));
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        frame_node node{};
        node.geometry = {20, 40, 140, 60};
        node.extents = extents_for(decoration_mode::server, false);
        node.shown = true;
        node.active = true;
        node.text = &chrome_env().text;
        lx::scene::scene_graph scene;
        lx::scene::output_view_config v{};
        v.logical = {0, 0, W, H};
        v.projection = {{0, 0}, 1.0, {W, H}};
        scene.set_output_view(0, v);
        (void)scene.root().add(&node);
        lx::compositor::frame_painter painter;
        const lx::rect2i c = close_rect(node.geometry);
        for (const deco_button b : {deco_button::hover, deco_button::pressed}) {
            node.button = b;
            lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
            scene.note_full_damage_output(0);
            LUMEN_CHECK(scene.commit_frame(0, b == deco_button::hover ? 1u : 2u));
            lx::runtime::set_current_affinity(lx::runtime::affinity::render);
            const auto& snap = scene.acquire_render_snapshot(0);
            lx::gfx::damage_region full{};
            full.full = true;
            auto p = painter.paint_gl(comp, nullptr, target->framebuffer(), W, H, snap, full,
                                      &chrome_env().atlas);
            LUMEN_CHECK(p && !p->paint.stream && p->paint.dropped == 0 && p->paint.lowered == 7);
            LUMEN_CHECK(p && p->composite.draws_skipped == 0);
            if (p && p->composite.out_fence_fd >= 0)
                ::close(p->composite.out_fence_fd);
            scene.release_render_frame(0, snap);
            std::vector<unsigned char> rgba(static_cast<std::size_t>(W * H) * 4u);
            LUMEN_CHECK(static_cast<bool>(comp.read_back(*target, rgba.data(),
                                                         static_cast<unsigned>(rgba.size()))));
            const auto at = [&](int x, int y) {
                const unsigned char* q = &rgba[static_cast<std::size_t>(y * W + x) * 4u];
                return (static_cast<std::uint32_t>(q[0]) << 16) | (static_cast<std::uint32_t>(q[1]) << 8) | q[2];
            };
            const std::uint32_t want = b == deco_button::hover ? k_deco_button_hover : k_deco_button_pressed;
            // Inside the circle, off the strokes: the button colour; the button's corner: the
            // title bar; the centre: the × (light).
            LUMEN_CHECK(at(c.x + 6, c.y + c.height / 2) == want);
            LUMEN_CHECK(at(c.x + 1, c.y + 1) == k_deco_title_active);
            LUMEN_CHECK(((at(c.x + c.width / 2, c.y + c.height / 2) >> 8) & 0xFFu) > 0xC0u);
        }
        (void)scene.root().remove(&node);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    }
    ::close(fd);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
