// Server-side frame geometry (lx.compositor:decoration): extents, frame rect, hit zones,
// the atlas, and frame-aware placement.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.gfx;
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

LUMEN_TEST(atlas_swatches_are_solid_and_premultiplied) {
    std::vector<std::uint32_t> px(k_deco_atlas_size * k_deco_atlas_size);
    build_deco_atlas(px.data());
    const auto at = [&](int x, int y) {
        return px[static_cast<std::size_t>(y) * k_deco_atlas_size + static_cast<std::size_t>(x)];
    };
    // Every pixel of a swatch's sampled centre has the same opaque colour.
    for (lx::rect2i sw : {deco_atlas::title_active, deco_atlas::title_inactive,
                          deco_atlas::border_active, deco_atlas::border_inactive}) {
        const lx::rect2i c = swatch_centre(sw);
        const std::uint32_t v = at(sw.x, sw.y);
        LUMEN_CHECK((v >> 24) == 0xFFu);
        for (int y = sw.y; y < sw.y + sw.height; ++y)
            for (int x = sw.x; x < sw.x + sw.width; ++x)
                LUMEN_CHECK(at(x, y) == v);
        LUMEN_CHECK(c.width == 2 && c.height == 2 && c.x == sw.x + 1 && c.y == sw.y + 1);
    }
    LUMEN_CHECK(at(0, 0) == 0xFF303033u); // title active #303033
    // Glyph: opaque centre stroke, transparent corners of the gap, premultiplied throughout.
    const lx::rect2i g = deco_atlas::close_light;
    LUMEN_CHECK((at(g.x + 5, g.y + 5) >> 24) > 200u);
    LUMEN_CHECK((at(g.x + 5, g.y + 0) >> 24) < 40u);
    // Circles: opaque centre, transparent corner.
    const lx::rect2i h = deco_atlas::circle_hover;
    LUMEN_CHECK((at(h.x + 10, h.y + 10) >> 24) == 0xFFu);
    LUMEN_CHECK(at(h.x, h.y) == 0u);
    for (std::uint32_t p : px) {
        const unsigned a = p >> 24;
        LUMEN_CHECK(((p >> 16) & 0xFFu) <= a && ((p >> 8) & 0xFFu) <= a && (p & 0xFFu) <= a);
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

/// Composites a frame node's draws on the CPU backend (the atlas registered as it is staged
/// there: native ARGB) into a black XRGB target, the way the render thread does.
struct composited {
    std::vector<std::uint32_t> px;
    int w = 0, h = 0;
    [[nodiscard]] std::uint32_t at(int x, int y) const {
        return px[static_cast<std::size_t>(y * w + x)] & 0x00FFFFFFu;
    }
};

composited composite_frame(const frame_node& node, int w, int h) {
    static std::vector<std::uint32_t> atlas(k_deco_atlas_size * k_deco_atlas_size);
    build_deco_atlas(atlas.data());
    lx::gfx::pixel_surface tex{};
    tex.pixels = reinterpret_cast<unsigned char*>(atlas.data());
    tex.width = tex.height = k_deco_atlas_size;
    tex.stride = k_deco_atlas_size * 4u;
    tex.format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    lx::gfx::cpu_compositor comp;
    (void)comp.register_texture(k_deco_atlas_texture_id, tex);

    lx::scene::draw_list list;
    node.emit_draws(list);
    std::vector<lx::gfx::blit_command> blits(list.size());
    for (unsigned i = 0; i < list.size(); ++i) {
        const auto& d = list.data()[i];
        blits[i].texture_id = d.texture.id();
        blits[i].dst = d.dst;
        blits[i].src = d.src;
        blits[i].clip = d.clip;
        blits[i].tint = d.tint;
        blits[i].opacity = d.opacity;
        blits[i].blend = d.blend;
    }
    composited out{std::vector<std::uint32_t>(static_cast<std::size_t>(w * h), 0u), w, h};
    lx::gfx::pixel_surface dst{};
    dst.pixels = reinterpret_cast<unsigned char*>(out.px.data());
    dst.width = static_cast<unsigned>(w);
    dst.height = static_cast<unsigned>(h);
    dst.stride = static_cast<unsigned>(w) * 4u;
    dst.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    (void)comp.composite(dst, lx::color::rgb(0.f, 0.f, 0.f), blits.data(),
                         static_cast<unsigned>(blits.size()), {});
    return out;
}

} // namespace

LUMEN_TEST(frame_draws_baked_colours_where_the_zones_are) {
    static bool atlas_ready = true;
    frame_node node{};
    node.geometry = {20, 40, 100, 50};
    node.extents = extents_for(decoration_mode::server, false);
    node.shown = true;
    node.active = true;
    node.atlas_ready = &atlas_ready;
    lx::scene::draw_list list;
    node.emit_draws(list);
    // Four borders, the title fill, the close glyph (no title text staged, no hover).
    LUMEN_CHECK(list.size() == 6);

    const composited a = composite_frame(node, 160, 120);
    const lx::rect2i g = node.geometry;
    LUMEN_CHECK(a.at(g.x + 30, g.y - 14) == 0x303033u);          // title bar, active
    LUMEN_CHECK(a.at(g.x + 30, g.y - T - 1) == 0x1B1B1Du);       // top border
    LUMEN_CHECK(a.at(g.x - 1, g.y + 20) == 0x1B1B1Du);           // left border
    LUMEN_CHECK(a.at(g.x + g.width, g.y + 20) == 0x1B1B1Du);     // right border
    LUMEN_CHECK(a.at(g.x + 30, g.y + g.height) == 0x1B1B1Du);    // bottom border
    LUMEN_CHECK(a.at(g.x + 30, g.y + 20) == 0u);                 // client area: untouched
    LUMEN_CHECK(a.at(g.x - 2, g.y + 20) == 0u);                  // outside: untouched
    // The close glyph's centre is light; the button's corner is the title colour.
    const lx::rect2i c = close_rect(g);
    const std::uint32_t mid = a.at(c.x + c.width / 2, c.y + c.height / 2);
    LUMEN_CHECK(((mid >> 16) & 0xFFu) > 0xC0u);
    LUMEN_CHECK(a.at(c.x + 1, c.y + 1) == 0x303033u);

    node.active = false;
    const composited i = composite_frame(node, 160, 120);
    LUMEN_CHECK(i.at(g.x + 30, g.y - 14) == 0x48484Cu);
    LUMEN_CHECK(i.at(g.x - 1, g.y + 20) == 0x2C2C30u);

    node.active = true;
    node.button = deco_button::hover;
    const composited h = composite_frame(node, 160, 120);
    // Inside the circle, off the glyph's strokes: the hover red.
    LUMEN_CHECK(h.at(c.x + 6, c.y + c.height / 2) == 0xE0443Eu);
    node.button = deco_button::pressed;
    const composited p = composite_frame(node, 160, 120);
    LUMEN_CHECK(p.at(c.x + 6, c.y + c.height / 2) == 0xB8322Du);

    // Maximized: no borders, the title bar directly above the client.
    node.button = deco_button::normal;
    node.extents = extents_for(decoration_mode::server, true);
    const composited m = composite_frame(node, 160, 120);
    LUMEN_CHECK(m.at(g.x - 1, g.y + 20) == 0u);
    LUMEN_CHECK(m.at(g.x + 30, g.y - 1) == 0x303033u);
    LUMEN_CHECK(m.at(g.x + 30, g.y - T - 1) == 0u);
}

LUMEN_TEST(frame_draws_nothing_before_the_atlas_or_when_hidden) {
    static bool atlas_ready = false;
    frame_node node{};
    node.geometry = {20, 40, 100, 50};
    node.extents = extents_for(decoration_mode::server, false);
    node.shown = true;
    node.atlas_ready = &atlas_ready;
    lx::scene::draw_list list;
    node.emit_draws(list);
    LUMEN_CHECK(list.size() == 0);
    atlas_ready = true;
    node.shown = false;
    node.emit_draws(list);
    LUMEN_CHECK(list.size() == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
