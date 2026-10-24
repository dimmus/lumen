// The compositor draws the UI primitive stream (ADR-010 §4.11, `lx.compositor:ui_composite`):
//   - a frame with no primitive takes the blit path it always took, byte for byte, and no
//     primitive renderer runs;
//   - a frame with server-side decorations (quad fills under a window) goes through the CPU
//     and GL primitive renderers and draws what the lowered path (GLES 2, Vulkan, capture)
//     draws, within G10 on GL;
//   - only opaque square quads occlude, and primitives are never culled;
//   - a turned output's primitives land where its draws do, with radii and borders turned.
#include <fcntl.h>
#include <unistd.h>

#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.compositor;

namespace {

constexpr int k_w = 256;
constexpr int k_h = 192;
constexpr unsigned k_window_texture = 0x1234u;
constexpr int k_tex = 64;

struct image {
    std::vector<std::uint32_t> px = std::vector<std::uint32_t>(static_cast<std::size_t>(k_w * k_h), 0u);
    [[nodiscard]] lx::gfx::pixel_surface surface() {
        lx::gfx::pixel_surface s{};
        s.pixels = reinterpret_cast<unsigned char*>(px.data());
        s.width = k_w;
        s.height = k_h;
        s.stride = k_w * 4u;
        s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
        return s;
    }
};

/// Max channel difference over the colour bytes (XRGB's fourth byte is undefined).
int max_diff(const image& a, const std::vector<std::uint32_t>& b, std::size_t* where = nullptr) {
    int worst = 0;
    for (std::size_t i = 0; i < a.px.size(); ++i)
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const int d = static_cast<int>((a.px[i] >> shift) & 0xFFu) -
                          static_cast<int>((b[i] >> shift) & 0xFFu);
            if ((d < 0 ? -d : d) > worst && where)
                *where = i;
            worst = std::max(worst, d < 0 ? -d : d);
        }
    return worst;
}

/// The window's texture (premultiplied ARGB words, a translucent band at the bottom) and
/// the decoration atlas, as the CPU path registers them.
struct textures {
    std::vector<std::uint32_t> window = std::vector<std::uint32_t>(k_tex * k_tex);
    std::vector<std::uint32_t> atlas =
        std::vector<std::uint32_t>(lx::compositor::k_deco_atlas_size * lx::compositor::k_deco_atlas_size);
    textures() {
        for (int y = 0; y < k_tex; ++y)
            for (int x = 0; x < k_tex; ++x) {
                const std::uint32_t a = y < 48 ? 255u : 128u;
                const std::uint32_t r = (static_cast<std::uint32_t>(x) * 4u * a) / 255u;
                const std::uint32_t g = (static_cast<std::uint32_t>(y) * 4u * a) / 255u;
                const std::uint32_t b = (96u * a) / 255u;
                window[static_cast<std::size_t>(y * k_tex + x)] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        lx::compositor::build_deco_atlas(atlas.data());
    }
    void register_cpu(lx::gfx::cpu_compositor& c) {
        lx::gfx::pixel_surface s{};
        s.pixels = reinterpret_cast<unsigned char*>(window.data());
        s.width = s.height = k_tex;
        s.stride = k_tex * 4u;
        s.format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
        LUMEN_CHECK(static_cast<bool>(c.register_texture(k_window_texture, s)));
        s.pixels = reinterpret_cast<unsigned char*>(atlas.data());
        s.width = s.height = lx::compositor::k_deco_atlas_size;
        s.stride = lx::compositor::k_deco_atlas_size * 4u;
        LUMEN_CHECK(static_cast<bool>(c.register_texture(lx::compositor::k_deco_atlas_texture_id, s)));
    }
    /// RGBA bytes of premultiplied ARGB words.
    static std::vector<unsigned char> rgba(const std::vector<std::uint32_t>& words) {
        std::vector<unsigned char> out(words.size() * 4);
        for (std::size_t i = 0; i < words.size(); ++i) {
            out[i * 4 + 0] = static_cast<unsigned char>(words[i] >> 16);
            out[i * 4 + 1] = static_cast<unsigned char>(words[i] >> 8);
            out[i * 4 + 2] = static_cast<unsigned char>(words[i]);
            out[i * 4 + 3] = static_cast<unsigned char>(words[i] >> 24);
        }
        return out;
    }
};

/// One output view with a window (a textured surface) and, optionally, its server-side
/// frame under it and a solid surface over part of the frame.
struct desktop {
    lx::scene::scene_graph scene{};
    lx::scene::surface_node window{lx::surface_id{7}};
    lx::scene::surface_node solid{lx::surface_id{8}};
    lx::compositor::frame_node frame{};
    bool atlas_ready = true;
    unsigned frame_index = 0;

    explicit desktop(bool decorated, lx::buffer_transform t = lx::buffer_transform::normal) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::scene::output_view_config v{};
        const bool turned = (static_cast<unsigned>(t) & 1u) != 0;
        v.logical = {0, 0, turned ? k_h : k_w, turned ? k_w : k_h};
        v.projection = {{0, 0}, 1.0, {k_w, k_h}, t};
        scene.set_output_view(0, v);
        const lx::rect2i geometry{40, 60, k_tex, k_tex};
        if (decorated) {
            frame.geometry = geometry;
            frame.extents = lx::compositor::extents_for(lx::compositor::decoration_mode::server, false);
            frame.shown = true;
            frame.active = true;
            frame.atlas_ready = &atlas_ready;
            (void)scene.root().add(&frame);
        }
        lx::gfx::imported_image img{};
        img.width = k_tex;
        img.height = k_tex;
        img.image_id = k_window_texture;
        window.attach(img);
        window.set_bounds({0, 0, k_tex, k_tex});
        window.set_transform({geometry.x, geometry.y});
        (void)scene.root().add(&window);
        // Over the frame's left half of the title bar: a solid, translucent panel.
        solid.set_bounds({0, 0, 50, 20});
        solid.set_transform({20, 36});
        solid.set_solid(true, lx::color::rgb(0.f, 0.25f, 0.f, 0.5f));
        (void)scene.root().add(&solid);
    }

    const lx::scene::immutable_frame_snapshot& publish() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        scene.note_full_damage_output(0);
        LUMEN_CHECK(scene.commit_frame(0, ++frame_index));
        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        return scene.acquire_render_snapshot(0);
    }
    void release(const lx::scene::immutable_frame_snapshot& s) {
        scene.release_render_frame(0, s);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    }
};

lx::gfx::damage_region full_region() {
    lx::gfx::damage_region r{};
    r.full = true;
    return r;
}

lx::gfx::damage_region two_rects() {
    lx::gfx::damage_region r{};
    r.add({30, 20, 60, 40});
    r.add({100, 110, 90, 50});
    return r;
}

} // namespace

LUMEN_TEST(frame_without_primitives_takes_the_blit_path_byte_for_byte) {
    textures tex{};
    lx::gfx::cpu_compositor comp;
    tex.register_cpu(comp);
    lx::gfx::ui_cpu_renderer ui;
    lx::compositor::frame_painter painter;
    desktop d{false};
    const auto& snap = d.publish();
    LUMEN_CHECK(!lx::compositor::frame_painter::has_primitives(snap));
    LUMEN_CHECK(snap.refs().empty() && snap.batches().empty());
    for (const lx::gfx::damage_region& region : {full_region(), two_rects()}) {
        image painted{};
        auto p = painter.paint_cpu(comp, &ui, painted.surface(), snap, region);
        LUMEN_CHECK(static_cast<bool>(p) && !p->paint.stream);
        // The path as it was: the draws as blits, composited region by region.
        image reference{};
        const auto blits = painter.surfaces(snap);
        LUMEN_CHECK(static_cast<bool>(comp.composite_region(reference.surface(), snap.clear_color(),
                                                            blits.data(),
                                                            static_cast<unsigned>(blits.size()),
                                                            region)));
        LUMEN_CHECK(painted.px == reference.px);
        // The lowered view of it is the same array.
        const auto flat = painter.flattened(snap);
        LUMEN_CHECK(flat.data() == blits.data() && flat.size() == blits.size());
    }
    LUMEN_CHECK(painter.stream_frames() == 0 && painter.lowered_frames() == 0 &&
                painter.blit_frames() == 2);
    d.release(snap);
}

LUMEN_TEST(decorated_frame_goes_through_the_cpu_primitive_renderer) {
    textures tex{};
    lx::gfx::cpu_compositor comp;
    tex.register_cpu(comp);
    lx::gfx::ui_cpu_renderer ui;
    lx::compositor::frame_painter painter;
    desktop d{true};
    const auto& snap = d.publish();
    LUMEN_CHECK(lx::compositor::frame_painter::has_primitives(snap));
    LUMEN_CHECK(snap.draws().primitive_count() == 5);
    for (const lx::gfx::damage_region& region : {full_region(), two_rects()}) {
        image streamed{};
        auto s = painter.paint_cpu(comp, &ui, streamed.surface(), snap, region);
        LUMEN_CHECK(static_cast<bool>(s) && s->paint.stream && s->ui.primitives_skipped == 0);
        image lowered{};
        auto l = painter.paint_cpu(comp, nullptr, lowered.surface(), snap, region);
        LUMEN_CHECK(static_cast<bool>(l) && !l->paint.stream && l->paint.lowered == 5 &&
                    l->paint.dropped == 0);
        LUMEN_CHECK(streamed.px == lowered.px);
    }
    image out{};
    LUMEN_CHECK(static_cast<bool>(painter.paint_cpu(comp, &ui, out.surface(), snap, full_region())));
    const auto at = [&](int x, int y) { return out.px[static_cast<std::size_t>(y * k_w + x)] & 0xFFFFFFu; };
    LUMEN_CHECK(at(100, 60 - 14) == 0x303033u); // title bar right of the panel
    LUMEN_CHECK(at(39, 100) == 0x1B1B1Du);      // left border
    LUMEN_CHECK(at(0, 0) == 0u);                // the clear
    // The translucent panel blends over the title bar, in the compositor's linear light.
    const std::uint32_t over = at(30, 40);
    LUMEN_CHECK(over != 0x303033u && ((over >> 8) & 0xFFu) > 0x30u);
    LUMEN_CHECK(painter.stream_frames() == 3 && painter.lowered_frames() == 2);
    d.release(snap);
}

// A decorated frame whose window texture was never registered: the stream path reports the
// skipped surface in `composite.draws_skipped`, as the blit path does, so the compositor's
// unregistered-texture warning fires on stream frames too.
LUMEN_TEST(stream_frames_report_unregistered_surfaces) {
    lx::gfx::cpu_compositor comp; // nothing registered
    lx::gfx::ui_cpu_renderer ui;
    lx::compositor::frame_painter painter;
    desktop d{true};
    const auto& snap = d.publish();
    LUMEN_CHECK(lx::compositor::frame_painter::has_primitives(snap));
    image streamed{};
    auto s = painter.paint_cpu(comp, &ui, streamed.surface(), snap, full_region());
    LUMEN_CHECK(static_cast<bool>(s) && s->paint.stream);
    LUMEN_CHECK(s && s->ui.surface_draws_skipped > 0 && s->composite.draws_skipped > 0);
    image lowered{};
    auto l = painter.paint_cpu(comp, nullptr, lowered.surface(), snap, full_region());
    LUMEN_CHECK(static_cast<bool>(l) && l->composite.draws_skipped > 0);
    d.release(snap);
}

LUMEN_TEST(only_opaque_square_quads_occlude_and_primitives_are_never_culled) {
    lx::scene::draw_list list;
    list.begin({200, 200});
    lx::scene::draw_command below{};
    below.texture = lx::texture_id{1};
    below.dst = {20, 20, 40, 40};
    below.blend = lx::blend_mode::opaque;
    list.push(below);
    // A solid square quad over it hides it.
    lx::gfx::quad_instance q{};
    q.bounds[0] = 10.f;
    q.bounds[1] = 10.f;
    q.bounds[2] = 80.f;
    q.bounds[3] = 80.f;
    const auto bg = lx::gfx::premultiplied_half(lx::color::rgb(0.2f, 0.2f, 0.2f, 1.f));
    q.background[0] = q.background_end[0] = bg.r;
    q.background[1] = q.background_end[1] = bg.g;
    q.background[2] = q.background_end[2] = bg.b;
    q.background[3] = q.background_end[3] = bg.a;
    list.push_quad(q, {10, 10, 80, 80}, list.orders().assign({10, 10, 80, 80}));
    // A rounded one, a translucent one and a fractional one do not.
    lx::scene::draw_command under_rounded = below;
    under_rounded.dst = {120, 20, 40, 40};
    list.push(under_rounded);
    lx::gfx::quad_instance r = q;
    r.bounds[0] = 110.f;
    r.radii[0] = lx::gfx::to_half(4.f);
    list.push_quad(r, {110, 10, 80, 80}, list.orders().assign({110, 10, 80, 80}));
    lx::scene::draw_command under_fraction = below;
    under_fraction.dst = {20, 120, 40, 40};
    list.push(under_fraction);
    lx::gfx::quad_instance f = q;
    f.bounds[1] = 110.5f;
    list.push_quad(f, {10, 110, 80, 81}, list.orders().assign({10, 110, 80, 81}));
    // A draw over the first quad: shown, and the quad under it stays in the stream.
    lx::scene::draw_command above = below;
    above.dst = {0, 0, 100, 100};
    list.push(above);
    bool shown[8]{};
    const unsigned marked = lx::scene::mark_occlusion(list, {0, 0, 200, 200}, shown);
    LUMEN_CHECK(marked == 1);
    LUMEN_CHECK(list.data()[0].occluded && !shown[0]);
    LUMEN_CHECK(!list.data()[1].occluded && !list.data()[2].occluded && !list.data()[3].occluded);
    list.batch_for_render();
    unsigned quads = 0;
    for (const lx::gfx::primitive_ref& ref : list.refs())
        quads += ref.kind == lx::gfx::primitive_kind::quad ? 1u : 0u;
    LUMEN_CHECK(quads == 3);
    // The span form (no primitives) is the pass it always was.
    lx::scene::draw_command plain[2] = {below, above};
    LUMEN_CHECK(lx::scene::mark_occlusion(std::span<lx::scene::draw_command>{plain, 2},
                                          {0, 0, 200, 200}, nullptr) == 1);
}

LUMEN_TEST(turned_output_turns_primitives_with_their_draws) {
    // The decorated desktop on a 90° output: rendering it equals rendering the content
    // space and turning the pixels.
    textures tex{};
    lx::gfx::cpu_compositor comp;
    tex.register_cpu(comp);
    lx::gfx::ui_cpu_renderer ui;
    lx::compositor::frame_painter painter;
    for (const lx::buffer_transform t :
         {lx::buffer_transform::rotate_90, lx::buffer_transform::rotate_180,
          lx::buffer_transform::flipped_270}) {
        desktop turned{true, t};
        const auto& snap = turned.publish();
        LUMEN_CHECK(snap.draws().primitive_count() == 5);
        image out{};
        LUMEN_CHECK(static_cast<bool>(painter.paint_cpu(comp, &ui, out.surface(), snap, full_region())));
        // Every quad's stream bounds match its instance bounds, inside the output.
        for (const lx::gfx::quad_instance& q : snap.draws().quad_instances())
            LUMEN_CHECK(q.bounds[0] >= 0.f && q.bounds[1] >= 0.f && q.bounds[0] + q.bounds[2] <= k_w &&
                        q.bounds[1] + q.bounds[3] <= k_h);
        // The window's top-left texel (the title bar sits above it in content space) and
        // the title bar pixel above it land where transform_rect puts them.
        const bool swap = (static_cast<unsigned>(t) & 1u) != 0;
        const int cw = swap ? k_h : k_w;
        const int ch = swap ? k_w : k_h;
        const lx::rect2i title_px = lx::gfx::transform_rect({100, 60 - 14, 1, 1}, t, cw, ch);
        const lx::rect2i border_px = lx::gfx::transform_rect({39, 100, 1, 1}, t, cw, ch);
        const auto at = [&](lx::rect2i r) {
            return out.px[static_cast<std::size_t>(r.y * k_w + r.x)] & 0xFFFFFFu;
        };
        LUMEN_CHECK(at(title_px) == 0x303033u);
        LUMEN_CHECK(at(border_px) == 0x1B1B1Du);
        turned.release(snap);
    }
    // Radii and borders follow their corners and edges: a quad with one rounded corner and
    // one bordered edge, drawn in content space and turned pixel by pixel, matches the
    // turned quad drawn in output space.
    for (unsigned ti = 1; ti < 8; ++ti) {
        const auto t = static_cast<lx::buffer_transform>(ti);
        const bool swap = (ti & 1u) != 0;
        const int cw = swap ? 48 : 64;
        const int ch = swap ? 64 : 48;
        const auto draw = [&](lx::buffer_transform turn, std::vector<std::uint32_t>& px, int w, int h) {
            lx::scene::draw_list list;
            list.begin({cw, ch});
            lx::gfx::quad_instance q{};
            q.bounds[0] = 6.f;
            q.bounds[1] = 4.f;
            q.bounds[2] = 40.f;
            q.bounds[3] = 30.f;
            q.radii[0] = lx::gfx::to_half(10.f);
            q.border_widths[1] = lx::gfx::to_half(3.f);
            const auto bg = lx::gfx::premultiplied_half(lx::color::rgb(0.9f, 0.5f, 0.1f, 1.f));
            const auto bc = lx::gfx::premultiplied_half(lx::color::rgb(0.1f, 0.2f, 0.9f, 1.f));
            const std::uint16_t b4[4] = {bg.r, bg.g, bg.b, bg.a};
            const std::uint16_t c4[4] = {bc.r, bc.g, bc.b, bc.a};
            for (unsigned k = 0; k < 4; ++k) {
                q.background[k] = q.background_end[k] = b4[k];
                q.border_color[k] = c4[k];
            }
            list.push_quad(q, {6, 4, 40, 30}, list.orders().assign({6, 4, 40, 30}));
            list.transform_primitives(turn, {cw, ch});
            list.batch_for_render();
            px.assign(static_cast<std::size_t>(w * h), 0u);
            lx::gfx::pixel_surface s{};
            s.pixels = reinterpret_cast<unsigned char*>(px.data());
            s.width = static_cast<unsigned>(w);
            s.height = static_cast<unsigned>(h);
            s.stride = static_cast<unsigned>(w) * 4u;
            s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
            lx::gfx::ui_stream st{};
            st.refs = list.refs();
            st.batches = list.batches();
            st.quads = list.quad_instances();
            LUMEN_CHECK(static_cast<bool>(ui.draw(s, st, nullptr, full_region(), lx::color::rgb(0.f, 0.f, 0.f))));
        };
        std::vector<std::uint32_t> content, turned;
        draw(lx::buffer_transform::normal, content, cw, ch);
        const int ow = swap ? ch : cw;
        const int oh = swap ? cw : ch;
        draw(t, turned, ow, oh);
        int worst = 0;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const lx::rect2i o = lx::gfx::transform_rect({x, y, 1, 1}, t, cw, ch);
                const std::uint32_t a = content[static_cast<std::size_t>(y * cw + x)];
                const std::uint32_t b = turned[static_cast<std::size_t>(o.y * ow + o.x)];
                for (unsigned shift = 0; shift < 24; shift += 8) {
                    const int dd = static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu);
                    worst = std::max(worst, dd < 0 ? -dd : dd);
                }
            }
        if (worst > 1)
            std::printf("    transform %u: max channel error %d\n", ti, worst);
        LUMEN_CHECK(worst <= 1);
    }
}

// ── GL: the compositor's GLES 3.1 path and its GLES 2 lowering ──────────────────

LUMEN_TEST(gl_decorated_frame_matches_cpu_and_the_gles2_lowering) {
    const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::printf("    (gl skip: no render node)\n");
        return;
    }
    {
        auto device = lx::gfx::egl_device::create(fd);
        if (!device || device->is_software_renderer()) {
            std::printf("    (gl skip: %s)\n", device ? "software renderer" : device.get_error().message);
            ::close(fd);
            return;
        }
        std::printf("    gl: %s, GLES %d.%d\n", device->renderer(), device->gles_major(),
                    device->gles_minor());
        LUMEN_CHECK(device->gles_at_least(3, 1));
        lx::gfx::gl_compositor comp;
        LUMEN_CHECK(static_cast<bool>(comp.initialize(*device)));
        lx::gfx::ui_gl_renderer ui;
        const bool ui_ready = static_cast<bool>(ui.initialize());
        LUMEN_CHECK(ui_ready);
        textures tex{};
        const auto window_rgba = textures::rgba(tex.window);
        const auto atlas_rgba = textures::rgba(tex.atlas);
        LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(k_window_texture, k_tex, k_tex, window_rgba.data())));
        LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(lx::compositor::k_deco_atlas_texture_id,
                                                       lx::compositor::k_deco_atlas_size,
                                                       lx::compositor::k_deco_atlas_size,
                                                       atlas_rgba.data())));
        lx::gfx::cpu_compositor cpu;
        tex.register_cpu(cpu);
        lx::gfx::ui_cpu_renderer cpu_ui;
        auto target = lx::gfx::ui_gl_target::create(k_w, k_h);
        LUMEN_CHECK(static_cast<bool>(target));
        lx::compositor::frame_painter painter;
        for (const bool decorated : {false, true}) {
            desktop d{decorated};
            const auto& snap = d.publish();
            image reference{};
            LUMEN_CHECK(static_cast<bool>(painter.paint_cpu(cpu, &cpu_ui, reference.surface(), snap, full_region())));
            for (lx::gfx::ui_gl_renderer* r : {&ui, static_cast<lx::gfx::ui_gl_renderer*>(nullptr)}) {
                const unsigned long long stream_before = painter.stream_frames();
                auto p = painter.paint_gl(comp, r, target->framebuffer(), k_w, k_h, snap, full_region());
                LUMEN_CHECK(static_cast<bool>(p));
                if (p && p->composite.out_fence_fd >= 0)
                    ::close(p->composite.out_fence_fd);
                // The primitive renderer runs only for a frame with primitives.
                LUMEN_CHECK(painter.stream_frames() - stream_before ==
                            (decorated && r != nullptr ? 1u : 0u));
                std::vector<std::uint32_t> px(static_cast<std::size_t>(k_w * k_h));
                LUMEN_CHECK(static_cast<bool>(target->read_back(px)));
                // The GL compositor blends in encoded space, the CPU one in linear light:
                // they differ where the translucent panel, the window's band and the close
                // glyph's anti-aliased edges blend (as they always did), so the comparison
                // skips those, and holds everywhere else to G10's 3/255.
                image masked = reference;
                for (int y = 0; y < k_h; ++y)
                    for (int x = 0; x < k_w; ++x) {
                        const bool panel = x >= 20 && x < 70 && y >= 36 && y < 56;
                        const bool band = x >= 40 && x < 40 + k_tex && y >= 60 + 48 && y < 60 + k_tex;
                        const bool close = x >= 76 && x < 104 && y >= 32 && y < 60;
                        if (panel || band || close)
                            masked.px[static_cast<std::size_t>(y * k_w + x)] =
                                px[static_cast<std::size_t>(y * k_w + x)];
                    }
                std::size_t where = 0;
                const int worst = max_diff(masked, px, &where);
                std::printf("    decorated=%d %s: max channel error %d/255 vs CPU at (%d, %d): "
                            "cpu %06x gl %06x\n",
                            decorated, r ? "ui_gl" : "lowered (GLES 2)", worst,
                            static_cast<int>(where % k_w), static_cast<int>(where / k_w),
                            masked.px[where] & 0xFFFFFFu, px[where] & 0xFFFFFFu);
                LUMEN_CHECK(worst <= 3);
            }
            d.release(snap);
        }
        LUMEN_CHECK(painter.dropped_primitives() == 0);
    }
    ::close(fd);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
