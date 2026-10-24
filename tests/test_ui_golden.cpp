// ADR-010 Phase 1: the CPU reference renderer (`lx.gfx:ui_cpu`) against its committed golden
// images, and the pixel rules the images do not show on their own (damage, content masks,
// target formats, surfaces, the fast path).
//
// A change to tests/golden/ui/*.png is a reviewed change: regenerate with
//   LUMEN_UI_GOLDEN_UPDATE=1 ./build/release/test_ui_golden
// look at every image it rewrote, and commit them together with the change that moved them.
// The CPU output must match its references bit for bit; the GL and Vulkan stages compare
// against the same files through `ui_golden::compare_xrgb` with `k_tolerance_vs_cpu`.

#include "lumen_test.hpp"
#include "ui_golden.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.gfx;
import lx.scene;
import lx.ui;

namespace {

struct fixture {
    std::unique_ptr<lx::gfx::atlas> atlas = std::make_unique<lx::gfx::atlas>();
    ui_golden::tiles tiles{};
    fixture() { LUMEN_CHECK(ui_golden::make_tiles(*atlas, tiles)); }
};

[[nodiscard]] lx::gfx::pixel_surface surface_of(std::vector<std::uint32_t>& px, int w, int h,
                                                lx::pixel_format format) {
    lx::gfx::pixel_surface s{};
    s.pixels = reinterpret_cast<unsigned char*>(px.data());
    s.width = static_cast<unsigned>(w);
    s.height = static_cast<unsigned>(h);
    s.stride = static_cast<unsigned>(w) * 4u;
    s.format = static_cast<lx::fourcc>(format);
    return s;
}

[[nodiscard]] lx::gfx::damage_region full_region() {
    lx::gfx::damage_region d{};
    d.set_full();
    return d;
}

} // namespace

LUMEN_TEST(cpu_matches_committed_references) {
    fixture fx{};
    const bool update = ui_golden::update_requested();
    unsigned failed = 0;
    for (const ui_golden::golden_case& c : ui_golden::cases()) {
        ui_golden::frame f{};
        LUMEN_CHECK(ui_golden::render_cpu(c, fx.tiles, *fx.atlas, f));
        if (update) {
            const ui_golden::rgb_image img = ui_golden::to_rgb(f.px.data(), f.width, f.height, f.width);
            LUMEN_CHECK(ui_golden::write_png(ui_golden::reference_path(c.name), img));
            std::printf("    updated %s\n", ui_golden::reference_path(c.name).c_str());
            continue;
        }
        const ui_golden::diff d = ui_golden::compare_xrgb(c, f.px.data(), f.width, 0);
        if (!d.ok()) {
            ++failed;
            if (!d.reference_found)
                std::printf("    %s: no reference at %s (LUMEN_UI_GOLDEN_UPDATE=1 writes it)\n",
                            c.name, ui_golden::reference_path(c.name).c_str());
            else
                std::printf("    %s: %llu pixels differ, max channel error %d, first at (%d, %d)\n",
                            c.name, d.over, d.max_error, d.first_x, d.first_y);
        }
    }
    LUMEN_CHECK(failed == 0);
}

// What the GL and Vulkan stages will call: the tolerance admits a near miss and rejects a
// real one.
LUMEN_TEST(tolerant_compare_bounds_the_channel_error) {
    fixture fx{};
    const ui_golden::golden_case& c = ui_golden::cases()[0];
    ui_golden::frame f{};
    LUMEN_CHECK(ui_golden::render_cpu(c, fx.tiles, *fx.atlas, f));
    if (ui_golden::update_requested())
        return;
    for (std::size_t i = 0; i < f.px.size(); i += 7)
        f.px[i] = (f.px[i] & 0xFFFFFF00u) | ((f.px[i] & 0xFFu) > 252u ? (f.px[i] & 0xFFu) - 3u
                                                                     : (f.px[i] & 0xFFu) + 3u);
    const ui_golden::diff near = ui_golden::compare_xrgb(c, f.px.data(), f.width,
                                                         ui_golden::k_tolerance_vs_cpu);
    LUMEN_CHECK(near.ok() && near.max_error == 3);
    f.px[100] ^= 0x00800000u;
    const ui_golden::diff far = ui_golden::compare_xrgb(c, f.px.data(), f.width,
                                                        ui_golden::k_tolerance_vs_cpu);
    LUMEN_CHECK(!far.ok() && far.over == 1 && far.first_y == 100 / f.width);
}

LUMEN_TEST(fast_path_is_bit_identical) {
    fixture fx{};
    for (const ui_golden::golden_case& c : ui_golden::cases()) {
        ui_golden::frame fast{};
        ui_golden::frame exact{};
        LUMEN_CHECK(ui_golden::render_cpu(c, fx.tiles, *fx.atlas, fast, true));
        LUMEN_CHECK(ui_golden::render_cpu(c, fx.tiles, *fx.atlas, exact, false));
        LUMEN_CHECK(fast.px == exact.px);
    }
}

// Damage: overlapping rects that cover the target repaint it identically, and a partial
// rect writes exactly its pixels.
LUMEN_TEST(damage_rects_repaint_identically) {
    fixture fx{};
    for (const ui_golden::golden_case& c : ui_golden::cases()) {
        ui_golden::frame reference{};
        LUMEN_CHECK(ui_golden::render_cpu(c, fx.tiles, *fx.atlas, reference));
        auto list = std::make_unique<lx::scene::draw_list>();
        ui_golden::build(c, fx.tiles, *list);
        const lx::gfx::ui_stream stream = ui_golden::stream_of(*list);
        lx::gfx::ui_cpu_renderer r{};

        std::vector<std::uint32_t> px(reference.px.size(), 0xDEADBEEFu);
        lx::gfx::damage_region thirds{};
        const int w = c.width;
        const int h = c.height;
        thirds.rects[0] = {0, 0, w / 2 + 3, h};
        thirds.rects[1] = {w / 2 - 5, 0, w - (w / 2 - 5), h / 2 + 1};
        thirds.rects[2] = {w / 3, h / 2 - 2, w, h};
        thirds.count = 3;
        auto drawn = r.draw(surface_of(px, w, h, lx::pixel_format::xrgb8888), stream,
                            fx.atlas.get(), thirds, ui_golden::premultiplied(c.clear));
        LUMEN_CHECK(static_cast<bool>(drawn) && drawn->rects == 3);
        LUMEN_CHECK(px == reference.px);

        std::vector<std::uint32_t> part(reference.px.size(), 0x01020304u);
        lx::gfx::damage_region one{};
        const lx::rect2i rect{w / 4, h / 4, w / 3, h / 2};
        one.rects[0] = rect;
        one.count = 1;
        LUMEN_CHECK(static_cast<bool>(r.draw(surface_of(part, w, h, lx::pixel_format::xrgb8888),
                                             stream, fx.atlas.get(), one,
                                             ui_golden::premultiplied(c.clear))));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const bool inside = x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
                                    y < rect.y + rect.height;
                LUMEN_CHECK(part[i] == (inside ? reference.px[i] : 0x01020304u));
            }
    }
}

LUMEN_TEST(empty_damage_draws_nothing) {
    fixture fx{};
    const ui_golden::golden_case& c = ui_golden::cases()[0];
    auto list = std::make_unique<lx::scene::draw_list>();
    ui_golden::build(c, fx.tiles, *list);
    std::vector<std::uint32_t> px(static_cast<std::size_t>(c.width) * c.height, 0x55555555u);
    lx::gfx::ui_cpu_renderer r{};
    auto drawn = r.draw(surface_of(px, c.width, c.height, lx::pixel_format::xrgb8888),
                        ui_golden::stream_of(*list), fx.atlas.get(), {}, lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn) && drawn->rects == 0);
    for (const std::uint32_t p : px)
        LUMEN_CHECK(p == 0x55555555u);
}

// A batch whose bounds miss the damage rect is skipped whole.
LUMEN_TEST(batches_outside_damage_are_skipped) {
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({100, 40});
    {
        lx::ui::draw_scope s{*list, {0, 0, 100, 40}};
        s.quad({4, 4, 20, 20}, ui_golden::solid(ui_golden::k_accent));
        s.shadow({60, 4, 20, 20}, ui_golden::shadow_of(0.5f, 2.f, 0.f));
    }
    list->batch_for_render();
    LUMEN_CHECK(list->batches().size() == 2);
    std::vector<std::uint32_t> px(100 * 40, 0u);
    lx::gfx::damage_region d{};
    d.rects[0] = {0, 0, 30, 30};
    d.count = 1;
    lx::gfx::ui_cpu_renderer r{};
    auto drawn = r.draw(surface_of(px, 100, 40, lx::pixel_format::xrgb8888),
                        ui_golden::stream_of(*list), nullptr, d, lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn));
    LUMEN_CHECK(drawn->batches_drawn == 1 && drawn->batches_skipped == 1 &&
                drawn->primitives_drawn == 1);
}

// An opaque quad on whole pixels covers exactly its bounds; a content mask cuts at pixel
// centres; the shadow reaches 3σ + spread and no further.
LUMEN_TEST(coverage_follows_pixel_centres) {
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({64, 64});
    {
        lx::ui::draw_scope s{*list, {0, 0, 64, 64}};
        s.quad({10, 12, 7, 5}, ui_golden::solid(lx::color::rgb(1.f, 0.f, 0.f)));
        s.push_clip({30, 0, 5, 64});
        s.quad({28, 30, 10, 10}, ui_golden::solid(lx::color::rgb(0.f, 1.f, 0.f)));
        s.pop();
    }
    list->batch_for_render();
    std::vector<std::uint32_t> px(64 * 64, 0xFF000000u);
    lx::gfx::ui_cpu_renderer r{};
    LUMEN_CHECK(static_cast<bool>(r.draw(surface_of(px, 64, 64, lx::pixel_format::xrgb8888),
                                         ui_golden::stream_of(*list), nullptr, full_region())));
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const std::uint32_t p = px[static_cast<std::size_t>(y) * 64 + x];
            const bool red = x >= 10 && x < 17 && y >= 12 && y < 17;
            const bool green = x >= 30 && x < 35 && y >= 30 && y < 40;
            LUMEN_CHECK(p == (red ? 0xFFFF0000u : green ? 0xFF00FF00u : 0xFF000000u));
        }

    auto shadow = std::make_unique<lx::scene::draw_list>();
    shadow->begin({64, 64});
    {
        lx::ui::draw_scope s{*shadow, {0, 0, 64, 64}};
        s.shadow({24, 24, 16, 16}, ui_golden::shadow_of(1.f, 2.f, 0.f, 1.f));
    }
    shadow->batch_for_render();
    std::vector<std::uint32_t> sp(64 * 64, 0xFFFFFFFFu);
    LUMEN_CHECK(static_cast<bool>(r.draw(surface_of(sp, 64, 64, lx::pixel_format::xrgb8888),
                                         ui_golden::stream_of(*shadow), nullptr, full_region())));
    // Reach 3 · 2 + 1 = 7 px: x in [17, 47) may be touched, x = 16 and 47 never.
    for (int y = 0; y < 64; ++y) {
        LUMEN_CHECK(sp[static_cast<std::size_t>(y) * 64 + 16] == 0xFFFFFFFFu);
        LUMEN_CHECK(sp[static_cast<std::size_t>(y) * 64 + 47] == 0xFFFFFFFFu);
    }
    // The centre of the box is (nearly) fully shadowed.
    LUMEN_CHECK((sp[32 * 64 + 32] & 0xFFu) < 3u);
}

// ARGB8888 keeps coverage in alpha (premultiplied); XRGB8888 writes 255 in the X byte.
LUMEN_TEST(target_formats_and_alpha) {
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({16, 16});
    {
        lx::ui::draw_scope s{*list, {0, 0, 16, 16}};
        s.quad({0, 0, 8, 8}, ui_golden::solid(lx::color::rgb(1.f, 1.f, 1.f, 0.5f)));
    }
    list->batch_for_render();
    lx::gfx::ui_cpu_renderer r{};
    std::vector<std::uint32_t> argb(16 * 16, 0u);
    LUMEN_CHECK(static_cast<bool>(r.draw(surface_of(argb, 16, 16, lx::pixel_format::argb8888),
                                         ui_golden::stream_of(*list), nullptr, full_region(),
                                         lx::color::rgb(0.f, 0.f, 0.f, 0.f))));
    LUMEN_CHECK(argb[0] == 0x80808080u);
    LUMEN_CHECK(argb[15] == 0u);
    std::vector<std::uint32_t> xrgb(16 * 16, 0u);
    LUMEN_CHECK(static_cast<bool>(r.draw(surface_of(xrgb, 16, 16, lx::pixel_format::xrgb8888),
                                         ui_golden::stream_of(*list), nullptr, full_region(),
                                         lx::color::rgb(0.f, 0.f, 0.f, 0.f))));
    LUMEN_CHECK(xrgb[0] == 0xFF808080u && xrgb[15] == 0xFF000000u);
    std::vector<std::uint32_t> rgba(16 * 16, 0u);
    LUMEN_CHECK(!r.draw(surface_of(rgba, 16, 16, lx::pixel_format::rgba8888),
                        ui_golden::stream_of(*list), nullptr, full_region()));
}

// A sprite whose page is missing or of the wrong format is skipped, not drawn wrong.
LUMEN_TEST(sprites_need_their_page) {
    fixture fx{};
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({32, 32});
    {
        lx::ui::draw_scope s{*list, {0, 0, 32, 32}};
        // An RGBA8 page drawn as mono, and a page never created.
        s.sprite(lx::gfx::primitive_kind::mono_sprite, fx.tiles.image.page, {0, 0, 8, 8},
                 fx.tiles.image.uv(), ui_golden::k_text);
        s.sprite(lx::gfx::primitive_kind::mono_sprite, 3, {8, 8, 8, 8}, {0, 0, 8, 8},
                 ui_golden::k_text);
    }
    list->batch_for_render();
    std::vector<std::uint32_t> px(32 * 32, 0u);
    lx::gfx::ui_cpu_renderer r{};
    auto drawn = r.draw(surface_of(px, 32, 32, lx::pixel_format::xrgb8888),
                        ui_golden::stream_of(*list), fx.atlas.get(), full_region(),
                        lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn) && drawn->primitives_skipped == 2 &&
                drawn->primitives_drawn == 0);
}

// Surfaces go through `cpu_compositor::blend` in stream order: a solid surface draw between
// two quads lands between them.
LUMEN_TEST(surfaces_draw_in_stream_order) {
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({32, 32});
    {
        lx::ui::draw_scope s{*list, {0, 0, 32, 32}};
        s.quad({0, 0, 16, 16}, ui_golden::solid(lx::color::rgb(1.f, 0.f, 0.f)));
        s.fill({4, 4, 16, 16}, lx::color::rgb(0.f, 0.f, 1.f));
        s.quad({8, 8, 16, 16}, ui_golden::solid(lx::color::rgb(0.f, 1.f, 0.f, 0.5f)));
    }
    list->batch_for_render();
    std::vector<lx::gfx::blit_command> blits;
    for (unsigned i = 0; i < list->size(); ++i) {
        const lx::scene::draw_command& d = list->data()[i];
        lx::gfx::blit_command b{};
        b.texture_id = static_cast<unsigned>(d.texture.id());
        b.dst = d.dst;
        b.src = d.src;
        b.clip = d.clip;
        b.tint = d.tint;
        b.opacity = d.opacity;
        b.solid = d.solid;
        b.solid_color = d.solid_color;
        blits.push_back(b);
    }
    lx::gfx::ui_stream stream = ui_golden::stream_of(*list);
    stream.surfaces = blits;
    lx::gfx::cpu_compositor compositor{};
    lx::gfx::ui_cpu_renderer r{};
    std::vector<std::uint32_t> px(32 * 32, 0u);
    // Without a compositor the surface is skipped and counted.
    auto skipped = r.draw(surface_of(px, 32, 32, lx::pixel_format::xrgb8888), stream, nullptr,
                          full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(skipped) && skipped->primitives_skipped == 1);
    r.set_surface_compositor(&compositor);
    auto drawn = r.draw(surface_of(px, 32, 32, lx::pixel_format::xrgb8888), stream, nullptr,
                        full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn) && drawn->primitives_drawn == 3);
    LUMEN_CHECK(px[1 * 32 + 1] == 0xFFFF0000u);   // the first quad alone
    LUMEN_CHECK(px[5 * 32 + 5] == 0xFF0000FFu);   // the surface over it
    // The translucent green quad over the blue surface: 0.5 green + 0.5 blue.
    LUMEN_CHECK(px[10 * 32 + 10] == 0xFF008080u);
}

int main(int argc, char** argv) {
    // `draw_scope` and the atlas are UI affinity.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
