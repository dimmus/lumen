// ADR-010 Phase 1: the CPU reference renderer (`lx.gfx:ui_cpu`) against its committed golden
// images, and the pixel rules the images do not show on their own (damage, content masks,
// target formats, surfaces, the fast path).
//
// A change to tests/golden/ui/*.png is a reviewed change: regenerate with
//   LUMEN_UI_GOLDEN_UPDATE=1 ./build/release/test_ui_golden
// look at every image it rewrote, and commit them together with the change that moved them.
// The CPU output must match its references bit for bit; the GL and Vulkan stages compare
// against the same files through `ui_golden::compare_xrgb` with `k_tolerance_vs_cpu`.
//
// The `gl_` tests draw the same cases through `lx.gfx:ui_gl` (GLES 3.1) on the render node
// and hold them to G10 against the same references. They skip (pass) when there is no
// render node or no GLES 3.1 context; a software GL_RENDERER is used — this is
// correctness, not speed. LUMEN_UI_GL_DUMP=<dir> writes each case's GL rendering there as
// <case>-gl.png, to look at beside the reference.

#include "lumen_test.hpp"
#include "ui_golden.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

// ── GLES 3.1: lx.gfx:ui_gl against the same references (G10) ───────────────────

namespace {

/// The render node's EGL context and one renderer, for the whole run (an EGL context is
/// per thread; rebuilding one per test is pure overhead). Null when GL is not usable here.
struct gl_context {
    int fd = -1;
    lx::gfx::egl_device device{};
    lx::gfx::ui_gl_renderer renderer{};
};

gl_context* shared_gl() {
    static gl_context ctx{};
    static bool tried = false;
    static bool usable = false;
    if (tried)
        return usable ? &ctx : nullptr;
    tried = true;
    ctx.fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (ctx.fd < 0) {
        std::printf("    (gl skip: no render node)\n");
        return nullptr;
    }
    auto device = lx::gfx::egl_device::create(ctx.fd);
    if (!device) {
        std::printf("    (gl skip: %s)\n", device.get_error().message);
        return nullptr;
    }
    ctx.device = std::move(device).value();
    if (auto ok = ctx.renderer.initialize(); !ok) {
        std::printf("    (gl skip: %s)\n", ok.get_error().message);
        return nullptr;
    }
    const lx::gfx::ui_gl_caps& caps = ctx.renderer.caps();
    std::printf("    gl: %s | %s | fetch %s | ring %s | timer query %d | program binary formats %d%s\n",
                caps.renderer, caps.version,
                caps.fetch == lx::gfx::ui_gl_fetch::attributes ? "attributes" : "storage",
                caps.persistent_ring ? "persistent" : "mapped-range",
                caps.timer_query ? 1 : 0, caps.program_binary_formats,
                caps.software ? " | software" : "");
    usable = true;
    return &ctx;
}

/// One case drawn through `r` into a fresh target of its size, read back as XRGB words.
[[nodiscard]] bool render_gl(lx::gfx::ui_gl_renderer& r, const ui_golden::golden_case& c,
                             const ui_golden::tiles& t, lx::gfx::atlas& a,
                             std::vector<std::uint32_t>& out, const lx::gfx::damage_region& damage,
                             lx::gfx::ui_gl_draw_stats* stats_out = nullptr) {
    auto list = std::make_unique<lx::scene::draw_list>();
    ui_golden::build(c, t, *list);
    auto target = lx::gfx::ui_gl_target::create(static_cast<unsigned>(c.width),
                                                static_cast<unsigned>(c.height));
    if (!target)
        return false;
    auto drawn = r.draw(*target, ui_golden::stream_of(*list), &a, damage,
                        ui_golden::premultiplied(c.clear));
    if (!drawn)
        return false;
    if (stats_out)
        *stats_out = *drawn;
    out.assign(static_cast<std::size_t>(c.width) * c.height, 0u);
    return static_cast<bool>(target->read_back(out));
}

} // namespace

LUMEN_TEST(gl_matches_committed_references) {
    gl_context* gl = shared_gl();
    if (!gl || ui_golden::update_requested())
        return;
    fixture fx{};
    unsigned failed = 0;
    for (const ui_golden::golden_case& c : ui_golden::cases()) {
        std::vector<std::uint32_t> px;
        lx::gfx::ui_gl_draw_stats stats{};
        LUMEN_CHECK(render_gl(gl->renderer, c, fx.tiles, *fx.atlas, px, full_region(), &stats));
        const ui_golden::diff d =
            ui_golden::compare_xrgb(c, px.data(), c.width, ui_golden::k_tolerance_vs_cpu);
        std::printf("    %-15s max channel error %d/255 vs CPU, %u draw(s), %llu instance bytes\n",
                    c.name, d.max_error, stats.draw_calls,
                    static_cast<unsigned long long>(stats.instance_bytes));
        if (const char* dump = std::getenv("LUMEN_UI_GL_DUMP"); dump && *dump) {
            std::string path = std::string(dump) + "/" + c.name + "-gl.png";
            (void)ui_golden::write_png(path, ui_golden::to_rgb(px.data(), c.width, c.height, c.width));
        }
        if (!d.ok()) {
            ++failed;
            std::printf("    %s: %llu pixels past %d/255, first at (%d, %d)\n", c.name, d.over,
                        ui_golden::k_tolerance_vs_cpu, d.first_x, d.first_y);
        }
        LUMEN_CHECK(stats.primitives_skipped == 0 && stats.overflow_drops == 0);
    }
    LUMEN_CHECK(failed == 0);
}

// Damage: overlapping rects that cover the target repaint it bit for bit as the full draw
// does, and a partial rect writes exactly its pixels.
LUMEN_TEST(gl_damage_rects_repaint_identically) {
    gl_context* gl = shared_gl();
    if (!gl)
        return;
    fixture fx{};
    for (const ui_golden::golden_case& c : ui_golden::cases()) {
        std::vector<std::uint32_t> reference;
        LUMEN_CHECK(render_gl(gl->renderer, c, fx.tiles, *fx.atlas, reference, full_region()));
        const int w = c.width;
        const int h = c.height;
        lx::gfx::damage_region thirds{};
        thirds.rects[0] = {0, 0, w / 2 + 3, h};
        thirds.rects[1] = {w / 2 - 5, 0, w - (w / 2 - 5), h / 2 + 1};
        thirds.rects[2] = {w / 3, h / 2 - 2, w, h};
        thirds.count = 3;
        std::vector<std::uint32_t> px;
        lx::gfx::ui_gl_draw_stats stats{};
        LUMEN_CHECK(render_gl(gl->renderer, c, fx.tiles, *fx.atlas, px, thirds, &stats));
        LUMEN_CHECK(stats.rects == 3);
        LUMEN_CHECK(px == reference);

        // A target filled with one colour, then one rect drawn: only the rect changes.
        auto list = std::make_unique<lx::scene::draw_list>();
        ui_golden::build(c, fx.tiles, *list);
        auto target = lx::gfx::ui_gl_target::create(static_cast<unsigned>(w), static_cast<unsigned>(h));
        LUMEN_CHECK(static_cast<bool>(target));
        const lx::color fill = lx::color::rgb(1.f / 255.f, 2.f / 255.f, 3.f / 255.f);
        LUMEN_CHECK(static_cast<bool>(gl->renderer.draw(*target, {}, nullptr, full_region(), fill)));
        lx::gfx::damage_region one{};
        const lx::rect2i rect{w / 4, h / 4, w / 3, h / 2};
        one.rects[0] = rect;
        one.count = 1;
        LUMEN_CHECK(static_cast<bool>(gl->renderer.draw(*target, ui_golden::stream_of(*list),
                                                        fx.atlas.get(), one,
                                                        ui_golden::premultiplied(c.clear))));
        std::vector<std::uint32_t> part(static_cast<std::size_t>(w) * h, 0u);
        LUMEN_CHECK(static_cast<bool>(target->read_back(part)));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const bool inside = x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
                                    y < rect.y + rect.height;
                LUMEN_CHECK(part[i] == (inside ? reference[i] : 0xFF010203u));
            }
    }
}

// The other paths — instances from a storage buffer instead of attributes, a mapped-range
// ring instead of the persistent mapping, no timer query, no program cache — draw the same
// pixels.
LUMEN_TEST(gl_fallback_ring_matches) {
    gl_context* gl = shared_gl();
    if (!gl)
        return;
    lx::gfx::ui_gl_renderer fallback{};
    lx::gfx::ui_gl_options options{};
    options.fetch = gl->renderer.caps().fetch == lx::gfx::ui_gl_fetch::attributes
                        ? lx::gfx::ui_gl_fetch::storage_buffer
                        : lx::gfx::ui_gl_fetch::attributes;
    options.persistent_ring = false;
    options.timer_query = false;
    options.program_cache = false;
    LUMEN_CHECK(static_cast<bool>(fallback.initialize(options)));
    LUMEN_CHECK(!fallback.caps().persistent_ring && !fallback.caps().timer_query);
    fixture fx{};
    // Twice round the ring of each, so every slot of both is written and reused.
    for (unsigned pass = 0; pass < 2; ++pass)
        for (const ui_golden::golden_case& c : ui_golden::cases()) {
            std::vector<std::uint32_t> a;
            std::vector<std::uint32_t> b;
            LUMEN_CHECK(render_gl(gl->renderer, c, fx.tiles, *fx.atlas, a, full_region()));
            LUMEN_CHECK(render_gl(fallback, c, fx.tiles, *fx.atlas, b, full_region()));
            LUMEN_CHECK(a == b);
        }
}

// A tile written after the page went up reaches the GPU through the page's dirty rects
// alone, and draws what the CPU reference draws.
LUMEN_TEST(gl_atlas_dirty_rects_upload) {
    gl_context* gl = shared_gl();
    if (!gl)
        return;
    fixture fx{};
    const ui_golden::golden_case& c = ui_golden::cases()[7]; // sprites: uploads the pages whole
    std::vector<std::uint32_t> first;
    lx::gfx::ui_gl_draw_stats stats{};
    LUMEN_CHECK(render_gl(gl->renderer, c, fx.tiles, *fx.atlas, first, full_region(), &stats));
    LUMEN_CHECK(stats.atlas_uploads >= 2);
    LUMEN_CHECK(fx.atlas->dirty_rects(0).empty() && !fx.atlas->dirty_full(0));

    std::uint8_t checker[10 * 6];
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 10; ++x)
            checker[y * 10 + x] = ((x + y) & 1) ? 255 : 40;
    auto id = fx.atlas->insert(lx::gfx::atlas_format::r8, 10, 6, {checker, sizeof checker}, 10);
    LUMEN_CHECK(static_cast<bool>(id));
    lx::gfx::atlas_tile tile{};
    LUMEN_CHECK(fx.atlas->lookup(*id, tile));

    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({40, 24});
    {
        lx::ui::draw_scope s{*list, {0, 0, 40, 24}};
        s.sprite(lx::gfx::primitive_kind::mono_sprite, tile.page, {3, 4, 10, 6}, tile.uv(),
                 ui_golden::k_text);
        s.sprite(lx::gfx::primitive_kind::mono_sprite, tile.page, {16, 3, 20, 15}, tile.uv(),
                 ui_golden::k_accent);
    }
    list->batch_for_render();
    std::vector<std::uint32_t> cpu(40 * 24, 0u);
    lx::gfx::ui_cpu_renderer reference{};
    LUMEN_CHECK(static_cast<bool>(reference.draw(surface_of(cpu, 40, 24, lx::pixel_format::xrgb8888),
                                                 ui_golden::stream_of(*list), fx.atlas.get(),
                                                 full_region(), lx::color::rgb(0, 0, 0))));
    auto target = lx::gfx::ui_gl_target::create(40, 24);
    LUMEN_CHECK(static_cast<bool>(target));
    auto drawn = gl->renderer.draw(*target, ui_golden::stream_of(*list), fx.atlas.get(),
                                   full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn));
    // One rect, the tile's: not the page.
    LUMEN_CHECK(drawn->atlas_uploads == 1 && drawn->atlas_upload_bytes == 60);
    std::vector<std::uint32_t> px(40 * 24, 0u);
    LUMEN_CHECK(static_cast<bool>(target->read_back(px)));
    int worst = 0;
    for (std::size_t i = 0; i < px.size(); ++i)
        for (unsigned sh = 0; sh < 24; sh += 8) {
            const int e = std::abs(static_cast<int>((px[i] >> sh) & 0xFFu) -
                                   static_cast<int>((cpu[i] >> sh) & 0xFFu));
            worst = e > worst ? e : worst;
        }
    LUMEN_CHECK(worst <= ui_golden::k_tolerance_vs_cpu);
}

// A sprite whose page is missing or of the wrong format is skipped, not drawn wrong.
LUMEN_TEST(gl_sprites_need_their_page) {
    gl_context* gl = shared_gl();
    if (!gl)
        return;
    fixture fx{};
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({32, 32});
    {
        lx::ui::draw_scope s{*list, {0, 0, 32, 32}};
        s.sprite(lx::gfx::primitive_kind::mono_sprite, fx.tiles.image.page, {0, 0, 8, 8},
                 fx.tiles.image.uv(), ui_golden::k_text);
        s.sprite(lx::gfx::primitive_kind::mono_sprite, 3, {8, 8, 8, 8}, {0, 0, 8, 8},
                 ui_golden::k_text);
    }
    list->batch_for_render();
    auto target = lx::gfx::ui_gl_target::create(32, 32);
    LUMEN_CHECK(static_cast<bool>(target));
    auto drawn = gl->renderer.draw(*target, ui_golden::stream_of(*list), fx.atlas.get(),
                                   full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn) && drawn->primitives_skipped == 2 &&
                drawn->primitives_encoded == 0 && drawn->draw_calls == 0);
    std::vector<std::uint32_t> px(32 * 32, 1u);
    LUMEN_CHECK(static_cast<bool>(target->read_back(px)));
    for (const std::uint32_t p : px)
        LUMEN_CHECK(p == 0xFF000000u);
}

// Surfaces go through `gl_compositor::composite_into` in stream order, between the quads.
LUMEN_TEST(gl_surfaces_draw_in_stream_order) {
    gl_context* gl = shared_gl();
    if (!gl)
        return;
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
    auto target = lx::gfx::ui_gl_target::create(32, 32);
    LUMEN_CHECK(static_cast<bool>(target));
    // Without a compositor the surface is skipped and counted.
    auto skipped = gl->renderer.draw(*target, stream, nullptr, full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(skipped) && skipped->primitives_skipped == 1);
    lx::gfx::gl_compositor compositor{};
    LUMEN_CHECK(static_cast<bool>(compositor.initialize(gl->device)));
    gl->renderer.set_surface_compositor(&compositor);
    auto drawn = gl->renderer.draw(*target, stream, nullptr, full_region(), lx::color::rgb(0, 0, 0));
    gl->renderer.set_surface_compositor(nullptr);
    LUMEN_CHECK(static_cast<bool>(drawn) && drawn->draw_calls == 3 && drawn->primitives_skipped == 0);
    std::vector<std::uint32_t> px(32 * 32, 0u);
    LUMEN_CHECK(static_cast<bool>(target->read_back(px)));
    const auto near = [](std::uint32_t got, std::uint32_t want) {
        for (unsigned sh = 0; sh < 24; sh += 8)
            if (std::abs(static_cast<int>((got >> sh) & 0xFFu) - static_cast<int>((want >> sh) & 0xFFu)) > 1)
                return false;
        return true;
    };
    LUMEN_CHECK(near(px[1 * 32 + 1], 0xFFFF0000u));
    LUMEN_CHECK(near(px[5 * 32 + 5], 0xFF0000FFu));
    LUMEN_CHECK(near(px[10 * 32 + 10], 0xFF008080u));
}

// Program binaries: with a binary format, the first renderer writes one file per program and
// the next one loads all five from them, drawing the same pixels. (SVGA3D has no format; Mesa's
// llvmpipe has one.)
LUMEN_TEST(gl_program_binary_cache) {
    gl_context* gl = shared_gl();
    if (!gl)
        return;
    if (gl->renderer.caps().program_binary_formats <= 0) {
        std::printf("    (skip: GL_NUM_PROGRAM_BINARY_FORMATS is 0)\n");
        return;
    }
    char dir[] = "/tmp/lumen-gl-cache-XXXXXX";
    LUMEN_CHECK(::mkdtemp(dir) != nullptr);
    const char* old = std::getenv("XDG_CACHE_HOME");
    const std::string saved = old ? old : "";
    ::setenv("XDG_CACHE_HOME", dir, 1);
    fixture fx{};
    const ui_golden::golden_case& c = ui_golden::cases()[6];
    std::vector<std::uint32_t> cold_px;
    std::vector<std::uint32_t> warm_px;
    lx::gfx::ui_gl_setup_stats cold{};
    lx::gfx::ui_gl_setup_stats warm{};
    {
        lx::gfx::ui_gl_renderer r{};
        LUMEN_CHECK(static_cast<bool>(r.initialize()));
        cold = r.setup_stats();
        LUMEN_CHECK(render_gl(r, c, fx.tiles, *fx.atlas, cold_px, full_region()));
    }
    {
        lx::gfx::ui_gl_renderer r{};
        LUMEN_CHECK(static_cast<bool>(r.initialize()));
        warm = r.setup_stats();
        LUMEN_CHECK(render_gl(r, c, fx.tiles, *fx.atlas, warm_px, full_region()));
    }
    if (old)
        ::setenv("XDG_CACHE_HOME", saved.c_str(), 1);
    else
        ::unsetenv("XDG_CACHE_HOME");
    std::printf("    program setup: cold %.1f ms (%u written), warm %.1f ms (%u from cache)\n",
                static_cast<double>(cold.program_ns) / 1e6, cold.written,
                static_cast<double>(warm.program_ns) / 1e6, warm.from_cache);
    LUMEN_CHECK(cold.from_cache == 0 && cold.written == cold.programs);
    LUMEN_CHECK(warm.from_cache == warm.programs && warm.written == 0);
    LUMEN_CHECK(cold_px == warm_px);
    // A damaged binary is recompiled, not trusted.
    const std::string lumen_dir = std::string(dir) + "/lumen";
    for (const auto& entry : std::filesystem::directory_iterator(lumen_dir)) {
        std::FILE* f = std::fopen(entry.path().c_str(), "r+b");
        LUMEN_CHECK(f != nullptr);
        std::fseek(f, 64, SEEK_SET);
        const char junk[16] = "not a program!!";
        std::fwrite(junk, 1, sizeof junk, f);
        std::fclose(f);
    }
    ::setenv("XDG_CACHE_HOME", dir, 1);
    {
        lx::gfx::ui_gl_renderer r{};
        LUMEN_CHECK(static_cast<bool>(r.initialize()));
        std::vector<std::uint32_t> px;
        LUMEN_CHECK(render_gl(r, c, fx.tiles, *fx.atlas, px, full_region()));
        LUMEN_CHECK(px == cold_px);
    }
    if (old)
        ::setenv("XDG_CACHE_HOME", saved.c_str(), 1);
    else
        ::unsetenv("XDG_CACHE_HOME");
    std::filesystem::remove_all(dir);
}

// GPU timing: a timed frame reports its source and a non-zero duration once.
LUMEN_TEST(gl_gpu_timing) {
    gl_context* gl = shared_gl();
    if (!gl)
        return;
    fixture fx{};
    gl->renderer.set_gpu_timing(true);
    std::vector<std::uint32_t> px;
    LUMEN_CHECK(render_gl(gl->renderer, ui_golden::cases()[5], fx.tiles, *fx.atlas, px, full_region()));
    auto t = gl->renderer.wait_gpu_time();
    gl->renderer.set_gpu_timing(false);
    LUMEN_CHECK(static_cast<bool>(t));
    LUMEN_CHECK(t->source == (gl->renderer.caps().timer_query ? lx::gfx::ui_gl_timing::timer_query
                                                              : lx::gfx::ui_gl_timing::fence_wall));
    std::printf("    gpu time %llu ns (%s)%s\n", static_cast<unsigned long long>(t->ns),
                t->source == lx::gfx::ui_gl_timing::timer_query ? "timer query" : "fence-wall",
                t->disjoint ? " disjoint" : "");
    auto again = gl->renderer.wait_gpu_time();
    LUMEN_CHECK(static_cast<bool>(again) && again->source == lx::gfx::ui_gl_timing::none);
}

// ── Vulkan (`lx.gfx:ui_vk`) ─────────────────────────────────────────────────────
//
// The same cases through the Vulkan renderer on the device `device_selector` picks — on this
// host lavapipe, a CPU device: correctness only. Held to G10 against the CPU references (3/255)
// and against `ui_gl` (2/255) when both run. Skips (passes) without a Vulkan device.
// LUMEN_VULKAN_VALIDATION=1 loads VK_LAYER_KHRONOS_validation (when installed) with its log in
// a temporary file, and `vk_validation_clean` (last) requires that log to hold no error.
// LUMEN_UI_VK_DUMP=<dir> writes each case as <case>-vk.png.

namespace {

struct vk_context_fixture {
    lx::gfx::device device{};
    lx::gfx::ui_vk_renderer renderer{};
};

std::string g_vk_validation_log{};

/// Torn down by `main` before it returns, not by a static destructor: the loader and the
/// validation layer are libraries opened at run time, and their own state is gone by the
/// time this file's statics are destroyed.
std::unique_ptr<vk_context_fixture> g_vk{};

vk_context_fixture* shared_vk() {
    static bool tried = false;
    static bool usable = false;
    if (tried)
        return usable ? g_vk.get() : nullptr;
    tried = true;
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected || !selected->context().valid()) {
        std::printf("    (vk skip: no Vulkan device)\n");
        return nullptr;
    }
    g_vk = std::make_unique<vk_context_fixture>();
    vk_context_fixture& ctx = *g_vk;
    ctx.device = std::move(selected).value();
    if (auto ok = ctx.renderer.initialize(ctx.device.context()); !ok) {
        std::printf("    (vk skip: %s)\n", ok.get_error().message);
        return nullptr;
    }
    const lx::gfx::ui_vk_caps& caps = ctx.renderer.caps();
    std::printf("    vk: %s | api %u.%u | timestamps %d | pipeline cache %s%s\n", caps.device_name,
                caps.api_version >> 22u, (caps.api_version >> 12u) & 0x3FFu, caps.timestamps ? 1 : 0,
                caps.pipeline_cache ? caps.pipeline_cache_path : "off",
                caps.software ? " | software (lavapipe-class: correctness only)" : "");
    usable = true;
    return g_vk.get();
}

[[nodiscard]] bool render_vk(lx::gfx::ui_vk_renderer& r, const lx::gfx::vk_context& ctx,
                             const ui_golden::golden_case& c, const ui_golden::tiles& t,
                             lx::gfx::atlas& a, std::vector<std::uint32_t>& out,
                             const lx::gfx::damage_region& damage,
                             lx::gfx::ui_vk_draw_stats* stats_out = nullptr) {
    auto list = std::make_unique<lx::scene::draw_list>();
    ui_golden::build(c, t, *list);
    auto target = lx::gfx::ui_vk_target::create(ctx, static_cast<unsigned>(c.width),
                                                static_cast<unsigned>(c.height));
    if (!target)
        return false;
    auto drawn = r.draw(*target, ui_golden::stream_of(*list), &a, damage,
                        ui_golden::premultiplied(c.clear));
    if (!drawn) {
        std::printf("    vk draw failed: %s\n", drawn.get_error().message);
        return false;
    }
    if (stats_out)
        *stats_out = *drawn;
    out.assign(static_cast<std::size_t>(c.width) * c.height, 0u);
    const bool ok = static_cast<bool>(target->read_back(out));
    r.wait_idle(); // the target goes away here
    return ok;
}

[[nodiscard]] int max_channel_error(const std::vector<std::uint32_t>& a,
                                    const std::vector<std::uint32_t>& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        for (unsigned sh = 0; sh < 24; sh += 8) {
            const int e = std::abs(static_cast<int>((a[i] >> sh) & 0xFFu) -
                                   static_cast<int>((b[i] >> sh) & 0xFFu));
            worst = e > worst ? e : worst;
        }
    return worst;
}

} // namespace

LUMEN_TEST(vk_matches_committed_references) {
    vk_context_fixture* vk = shared_vk();
    if (!vk || ui_golden::update_requested())
        return;
    gl_context* gl = shared_gl();
    fixture fx{};
    fixture gl_fx{}; // its own atlas: each renderer clears the dirty lists it uploads
    unsigned failed = 0;
    for (const ui_golden::golden_case& c : ui_golden::cases()) {
        std::vector<std::uint32_t> px;
        lx::gfx::ui_vk_draw_stats stats{};
        LUMEN_CHECK(render_vk(vk->renderer, vk->device.context(), c, fx.tiles, *fx.atlas, px,
                              full_region(), &stats));
        const ui_golden::diff d =
            ui_golden::compare_xrgb(c, px.data(), c.width, ui_golden::k_tolerance_vs_cpu);
        int vs_gl = -1;
        if (gl) {
            std::vector<std::uint32_t> gl_px;
            LUMEN_CHECK(render_gl(gl->renderer, c, gl_fx.tiles, *gl_fx.atlas, gl_px, full_region()));
            vs_gl = max_channel_error(px, gl_px);
        }
        std::printf("    %-15s max channel error %d/255 vs CPU, %d/255 vs GL, %u draw(s), %llu "
                    "instance bytes\n",
                    c.name, d.max_error, vs_gl, stats.draw_calls,
                    static_cast<unsigned long long>(stats.instance_bytes));
        if (const char* dump = std::getenv("LUMEN_UI_VK_DUMP"); dump && *dump) {
            std::string path = std::string(dump) + "/" + c.name + "-vk.png";
            (void)ui_golden::write_png(path, ui_golden::to_rgb(px.data(), c.width, c.height, c.width));
        }
        if (!d.ok()) {
            ++failed;
            std::printf("    %s: %llu pixels past %d/255, first at (%d, %d)\n", c.name, d.over,
                        ui_golden::k_tolerance_vs_cpu, d.first_x, d.first_y);
        }
        if (vs_gl > 2) {
            ++failed;
            std::printf("    %s: %d/255 from GL, past G10's 2/255\n", c.name, vs_gl);
        }
        LUMEN_CHECK(stats.primitives_skipped == 0 && stats.overflow_drops == 0);
    }
    LUMEN_CHECK(failed == 0);
}

LUMEN_TEST(vk_damage_rects_repaint_identically) {
    vk_context_fixture* vk = shared_vk();
    if (!vk)
        return;
    const lx::gfx::vk_context ctx = vk->device.context();
    fixture fx{};
    for (const ui_golden::golden_case& c : ui_golden::cases()) {
        std::vector<std::uint32_t> reference;
        LUMEN_CHECK(render_vk(vk->renderer, ctx, c, fx.tiles, *fx.atlas, reference, full_region()));
        const int w = c.width;
        const int h = c.height;
        lx::gfx::damage_region thirds{};
        thirds.rects[0] = {0, 0, w / 2 + 3, h};
        thirds.rects[1] = {w / 2 - 5, 0, w - (w / 2 - 5), h / 2 + 1};
        thirds.rects[2] = {w / 3, h / 2 - 2, w, h};
        thirds.count = 3;
        std::vector<std::uint32_t> px;
        lx::gfx::ui_vk_draw_stats stats{};
        LUMEN_CHECK(render_vk(vk->renderer, ctx, c, fx.tiles, *fx.atlas, px, thirds, &stats));
        LUMEN_CHECK(stats.rects == 3);
        LUMEN_CHECK(px == reference);

        // A target filled with one colour, then one rect drawn: only the rect changes.
        auto list = std::make_unique<lx::scene::draw_list>();
        ui_golden::build(c, fx.tiles, *list);
        auto target = lx::gfx::ui_vk_target::create(ctx, static_cast<unsigned>(w),
                                                    static_cast<unsigned>(h));
        LUMEN_CHECK(static_cast<bool>(target));
        const lx::color fill = lx::color::rgb(1.f / 255.f, 2.f / 255.f, 3.f / 255.f);
        LUMEN_CHECK(static_cast<bool>(vk->renderer.draw(*target, {}, nullptr, full_region(), fill)));
        lx::gfx::damage_region one{};
        const lx::rect2i rect{w / 4, h / 4, w / 3, h / 2};
        one.rects[0] = rect;
        one.count = 1;
        LUMEN_CHECK(static_cast<bool>(vk->renderer.draw(*target, ui_golden::stream_of(*list),
                                                        fx.atlas.get(), one,
                                                        ui_golden::premultiplied(c.clear))));
        std::vector<std::uint32_t> part(static_cast<std::size_t>(w) * h, 0u);
        LUMEN_CHECK(static_cast<bool>(target->read_back(part)));
        vk->renderer.wait_idle();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const bool inside = x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
                                    y < rect.y + rect.height;
                LUMEN_CHECK(part[i] == (inside ? reference[i] : 0xFF010203u));
            }
    }
}

// Round the ring several times: every slot is written, waited for and reused, and draws the
// same pixels as the first time (stale instances or globals would show).
LUMEN_TEST(vk_ring_slots_reuse) {
    vk_context_fixture* vk = shared_vk();
    if (!vk)
        return;
    const lx::gfx::vk_context ctx = vk->device.context();
    fixture fx{};
    std::vector<std::vector<std::uint32_t>> first;
    for (unsigned pass = 0; pass < 3; ++pass)
        for (std::size_t i = 0; i < ui_golden::cases().size(); ++i) {
            std::vector<std::uint32_t> px;
            LUMEN_CHECK(render_vk(vk->renderer, ctx, ui_golden::cases()[i], fx.tiles, *fx.atlas, px,
                                  full_region()));
            if (pass == 0)
                first.push_back(std::move(px));
            else
                LUMEN_CHECK(px == first[i]);
        }
}

// A fresh atlas goes up whole (more than a slot's staging range: a transient buffer); a tile
// written after that reaches the GPU as one copy of one region, and draws what the CPU
// reference draws.
LUMEN_TEST(vk_atlas_dirty_rects_upload) {
    vk_context_fixture* vk = shared_vk();
    if (!vk)
        return;
    const lx::gfx::vk_context ctx = vk->device.context();
    fixture fx{};
    // A renderer that has seen no atlas: every page goes up whole.
    lx::gfx::ui_vk_renderer r{};
    LUMEN_CHECK(static_cast<bool>(r.initialize(ctx)));
    const ui_golden::golden_case& c = ui_golden::cases()[7]; // sprites: R8 and RGBA8 pages
    std::vector<std::uint32_t> first;
    lx::gfx::ui_vk_draw_stats stats{};
    LUMEN_CHECK(render_vk(r, ctx, c, fx.tiles, *fx.atlas, first, full_region(), &stats));
    std::printf("    first frame: %u copies, %u regions, %llu bytes%s\n", stats.atlas_copies,
                stats.atlas_uploads, static_cast<unsigned long long>(stats.atlas_upload_bytes),
                stats.atlas_transient ? ", transient staging" : "");
    LUMEN_CHECK(stats.atlas_copies >= 2 && stats.atlas_uploads == stats.atlas_copies);
    LUMEN_CHECK(stats.atlas_transient); // whole pages: more than a slot's staging range
    LUMEN_CHECK(fx.atlas->dirty_rects(0).empty() && !fx.atlas->dirty_full(0));
    LUMEN_CHECK(ui_golden::compare_xrgb(c, first.data(), c.width, ui_golden::k_tolerance_vs_cpu).ok());
    // Nothing dirty: nothing goes up; round the ring, so the transient buffer is freed.
    for (unsigned i = 0; i < 4; ++i) {
        std::vector<std::uint32_t> again;
        LUMEN_CHECK(render_vk(r, ctx, c, fx.tiles, *fx.atlas, again, full_region(), &stats));
        LUMEN_CHECK(stats.atlas_copies == 0 && stats.atlas_upload_bytes == 0 && again == first);
    }
    r.shutdown();

    std::uint8_t checker[10 * 6];
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 10; ++x)
            checker[y * 10 + x] = ((x + y) & 1) ? 255 : 40;
    auto id = fx.atlas->insert(lx::gfx::atlas_format::r8, 10, 6, {checker, sizeof checker}, 10);
    LUMEN_CHECK(static_cast<bool>(id));
    lx::gfx::atlas_tile tile{};
    LUMEN_CHECK(fx.atlas->lookup(*id, tile));

    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({40, 24});
    {
        lx::ui::draw_scope s{*list, {0, 0, 40, 24}};
        s.sprite(lx::gfx::primitive_kind::mono_sprite, tile.page, {3, 4, 10, 6}, tile.uv(),
                 ui_golden::k_text);
        s.sprite(lx::gfx::primitive_kind::mono_sprite, tile.page, {16, 3, 20, 15}, tile.uv(),
                 ui_golden::k_accent);
    }
    list->batch_for_render();
    std::vector<std::uint32_t> cpu(40 * 24, 0u);
    lx::gfx::ui_cpu_renderer reference{};
    LUMEN_CHECK(static_cast<bool>(reference.draw(surface_of(cpu, 40, 24, lx::pixel_format::xrgb8888),
                                                 ui_golden::stream_of(*list), fx.atlas.get(),
                                                 full_region(), lx::color::rgb(0, 0, 0))));
    auto target = lx::gfx::ui_vk_target::create(ctx, 40, 24);
    LUMEN_CHECK(static_cast<bool>(target));
    auto drawn = vk->renderer.draw(*target, ui_golden::stream_of(*list), fx.atlas.get(),
                                   full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn));
    // One copy of one rect, the tile's: not the page.
    LUMEN_CHECK(drawn->atlas_copies == 1 && drawn->atlas_uploads == 1 &&
                drawn->atlas_upload_bytes == 60 && !drawn->atlas_transient);
    std::vector<std::uint32_t> px(40 * 24, 0u);
    LUMEN_CHECK(static_cast<bool>(target->read_back(px)));
    vk->renderer.wait_idle();
    LUMEN_CHECK(max_channel_error(px, cpu) <= ui_golden::k_tolerance_vs_cpu);
}

// A sprite whose page is missing or of the wrong format is skipped, not drawn wrong.
LUMEN_TEST(vk_sprites_need_their_page) {
    vk_context_fixture* vk = shared_vk();
    if (!vk)
        return;
    fixture fx{};
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({32, 32});
    {
        lx::ui::draw_scope s{*list, {0, 0, 32, 32}};
        s.sprite(lx::gfx::primitive_kind::mono_sprite, fx.tiles.image.page, {0, 0, 8, 8},
                 fx.tiles.image.uv(), ui_golden::k_text);
        s.sprite(lx::gfx::primitive_kind::mono_sprite, 3, {8, 8, 8, 8}, {0, 0, 8, 8},
                 ui_golden::k_text);
    }
    list->batch_for_render();
    auto target = lx::gfx::ui_vk_target::create(vk->device.context(), 32, 32);
    LUMEN_CHECK(static_cast<bool>(target));
    auto drawn = vk->renderer.draw(*target, ui_golden::stream_of(*list), fx.atlas.get(),
                                   full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn) && drawn->primitives_skipped == 2 &&
                drawn->primitives_encoded == 0 && drawn->draw_calls == 0);
    std::vector<std::uint32_t> px(32 * 32, 1u);
    LUMEN_CHECK(static_cast<bool>(target->read_back(px)));
    vk->renderer.wait_idle();
    for (const std::uint32_t p : px)
        LUMEN_CHECK(p == 0xFF000000u);
}

// Surfaces go through `vulkan_compositor::record_blits` in stream order, between the quads,
// inside the UI renderer's own pass.
LUMEN_TEST(vk_surfaces_draw_in_stream_order) {
    vk_context_fixture* vk = shared_vk();
    if (!vk)
        return;
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
    auto target = lx::gfx::ui_vk_target::create(vk->device.context(), 32, 32);
    LUMEN_CHECK(static_cast<bool>(target));
    // Without a compositor the surface is skipped and counted.
    auto skipped = vk->renderer.draw(*target, stream, nullptr, full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(skipped) && skipped->primitives_skipped == 1);
    lx::gfx::vulkan_compositor compositor{};
    LUMEN_CHECK(static_cast<bool>(compositor.initialize(vk->device.context())));
    vk->renderer.set_surface_compositor(&compositor);
    auto drawn = vk->renderer.draw(*target, stream, nullptr, full_region(), lx::color::rgb(0, 0, 0));
    LUMEN_CHECK(static_cast<bool>(drawn) && drawn->draw_calls == 3 && drawn->primitives_skipped == 0);
    std::vector<std::uint32_t> px(32 * 32, 0u);
    LUMEN_CHECK(static_cast<bool>(target->read_back(px)));
    vk->renderer.wait_idle();
    vk->renderer.set_surface_compositor(nullptr);
    const auto near = [](std::uint32_t got, std::uint32_t want) {
        for (unsigned sh = 0; sh < 24; sh += 8)
            if (std::abs(static_cast<int>((got >> sh) & 0xFFu) - static_cast<int>((want >> sh) & 0xFFu)) > 1)
                return false;
        return true;
    };
    LUMEN_CHECK(near(px[1 * 32 + 1], 0xFFFF0000u));
    LUMEN_CHECK(near(px[5 * 32 + 5], 0xFF0000FFu));
    LUMEN_CHECK(near(px[10 * 32 + 10], 0xFF008080u));
}

// The pipeline cache on disk. A cold renderer writes it when the driver has data worth
// keeping, a warm one reads it (and writes nothing), both draw the same pixels; a file whose
// header names another device is not handed to the driver, and one naming this device is.
// The times are the device's — on lavapipe they are lavapipe numbers, and lavapipe keeps no
// pipelines in a VkPipelineCache (its data is a truncated 20-byte header): nothing is written
// there and warm is cold.
LUMEN_TEST(vk_pipeline_cache) {
    vk_context_fixture* vk = shared_vk();
    if (!vk)
        return;
    const lx::gfx::vk_context ctx = vk->device.context();
    char dir[] = "/tmp/lumen-vk-cache-XXXXXX";
    LUMEN_CHECK(::mkdtemp(dir) != nullptr);
    const char* old = std::getenv("XDG_CACHE_HOME");
    const std::string saved = old ? old : "";
    ::setenv("XDG_CACHE_HOME", dir, 1);
    fixture fx{};
    const ui_golden::golden_case& c = ui_golden::cases()[6];
    std::vector<std::uint32_t> cold_px;
    std::vector<std::uint32_t> warm_px;
    lx::gfx::ui_vk_setup_stats cold{};
    lx::gfx::ui_vk_setup_stats warm{};
    lx::gfx::ui_vk_caps caps{};
    {
        lx::gfx::ui_vk_renderer r{};
        LUMEN_CHECK(static_cast<bool>(r.initialize(ctx)));
        cold = r.setup_stats();
        caps = r.caps();
        LUMEN_CHECK(render_vk(r, ctx, c, fx.tiles, *fx.atlas, cold_px, full_region()));
    }
    {
        lx::gfx::ui_vk_renderer r{};
        LUMEN_CHECK(static_cast<bool>(r.initialize(ctx)));
        LUMEN_CHECK(render_vk(r, ctx, c, fx.tiles, *fx.atlas, warm_px, full_region()));
        r.shutdown();
        warm = r.setup_stats();
    }
    std::printf("    pipeline setup (%s%s): cold %.2f ms (%u pipelines, %u writes, %llu bytes), warm "
                "%.2f ms (%llu bytes read, %u writes), shader modules %.2f ms\n",
                caps.device_name, caps.software ? ", software" : "",
                static_cast<double>(cold.pipeline_ns) / 1e6, cold.pipelines, cold.cache_writes,
                static_cast<unsigned long long>(cold.cache_bytes_written),
                static_cast<double>(warm.pipeline_ns) / 1e6,
                static_cast<unsigned long long>(warm.cache_bytes_loaded), warm.cache_writes,
                static_cast<double>(cold.module_ns) / 1e6);
    LUMEN_CHECK(cold.cache_bytes_loaded == 0 && !cold.cache_rejected);
    LUMEN_CHECK(cold_px == warm_px);
    if (cold.cache_writes > 0) {
        LUMEN_CHECK(warm.cache_bytes_loaded > 0 && warm.cache_writes == 0 && !warm.cache_rejected);
    } else {
        std::printf("    (the driver returned no cache data that passes its own header check: "
                    "nothing persisted)\n");
        LUMEN_CHECK(!std::filesystem::exists(caps.pipeline_cache_path));
    }

    // A header naming this device is handed over; one naming another vendor is not.
    std::uint8_t header[32]{};
    const std::uint32_t words[4] = {32u, 1u, caps.vendor_id, caps.device_id};
    std::memcpy(header, words, sizeof words);
    for (unsigned i = 0; i < 16; ++i)
        header[16 + i] = static_cast<std::uint8_t>(
            std::stoul(std::string(caps.pipeline_cache_uuid + 2 * i, 2), nullptr, 16));
    const auto with_file = [&](const std::uint8_t* bytes, std::size_t n) {
        std::FILE* f = std::fopen(caps.pipeline_cache_path, "wb");
        LUMEN_CHECK(f != nullptr);
        std::fwrite(bytes, 1, n, f);
        std::fclose(f);
        lx::gfx::ui_vk_renderer r{};
        LUMEN_CHECK(static_cast<bool>(r.initialize(ctx)));
        const lx::gfx::ui_vk_setup_stats st = r.setup_stats();
        std::vector<std::uint32_t> px;
        LUMEN_CHECK(render_vk(r, ctx, c, fx.tiles, *fx.atlas, px, full_region()));
        LUMEN_CHECK(px == cold_px);
        return st;
    };
    const lx::gfx::ui_vk_setup_stats accepted = with_file(header, sizeof header);
    const std::uint32_t other_vendor = caps.vendor_id ^ 0xFFFFu;
    std::memcpy(header + 8, &other_vendor, sizeof other_vendor);
    const lx::gfx::ui_vk_setup_stats rejected = with_file(header, sizeof header);
    LUMEN_CHECK(accepted.cache_bytes_loaded == 32 && !accepted.cache_rejected);
    LUMEN_CHECK(rejected.cache_bytes_loaded == 0 && rejected.cache_rejected);

    if (old)
        ::setenv("XDG_CACHE_HOME", saved.c_str(), 1);
    else
        ::unsetenv("XDG_CACHE_HOME");
    std::filesystem::remove_all(dir);
}

// GPU timing: a timed frame reports its source and a duration once.
LUMEN_TEST(vk_gpu_timing) {
    vk_context_fixture* vk = shared_vk();
    if (!vk)
        return;
    fixture fx{};
    vk->renderer.set_gpu_timing(true);
    auto list = std::make_unique<lx::scene::draw_list>();
    const ui_golden::golden_case& c = ui_golden::cases()[5];
    ui_golden::build(c, fx.tiles, *list);
    auto target = lx::gfx::ui_vk_target::create(vk->device.context(), static_cast<unsigned>(c.width),
                                                static_cast<unsigned>(c.height));
    LUMEN_CHECK(static_cast<bool>(target));
    LUMEN_CHECK(static_cast<bool>(vk->renderer.draw(*target, ui_golden::stream_of(*list),
                                                    fx.atlas.get(), full_region(),
                                                    ui_golden::premultiplied(c.clear))));
    auto t = vk->renderer.wait_gpu_time();
    vk->renderer.set_gpu_timing(false);
    LUMEN_CHECK(static_cast<bool>(t));
    LUMEN_CHECK(t->source == (vk->renderer.caps().timestamps ? lx::gfx::ui_vk_timing::timestamp
                                                             : lx::gfx::ui_vk_timing::fence_wall));
    std::printf("    gpu time %llu ns (%s)%s\n", static_cast<unsigned long long>(t->ns),
                t->source == lx::gfx::ui_vk_timing::timestamp ? "timestamps" : "fence-wall",
                vk->renderer.caps().software ? " — a CPU device: not a GPU number" : "");
    auto again = vk->renderer.wait_gpu_time();
    LUMEN_CHECK(static_cast<bool>(again) && again->source == lx::gfx::ui_vk_timing::none);
    vk->renderer.wait_idle();
}

// Last: with LUMEN_VULKAN_VALIDATION=1, the validation layer's log holds no error.
LUMEN_TEST(vk_validation_clean) {
    if (g_vk_validation_log.empty())
        return;
    if (vk_context_fixture* vk = shared_vk())
        vk->renderer.wait_idle();
    std::FILE* f = std::fopen(g_vk_validation_log.c_str(), "r");
    unsigned errors = 0;
    unsigned lines = 0;
    if (f) {
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            ++lines;
            const bool error = std::strstr(line, "Validation Error") != nullptr;
            if (error || std::strstr(line, "Validation Warning")) {
                if (lines < 32)
                    std::printf("    %s", line);
                errors += error ? 1u : 0u;
            }
        }
        std::fclose(f);
    }
    std::printf("    validation layer log %s: %u lines, %u errors\n", g_vk_validation_log.c_str(),
                lines, errors);
    LUMEN_CHECK(errors == 0);
}

int main(int argc, char** argv) {
    // `draw_scope` and the atlas are UI affinity.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    // LUMEN_VULKAN_VALIDATION=1: the Khronos validation layer for every Vulkan instance this
    // process makes, logging to a file `vk_validation_clean` reads.
    if (const char* v = std::getenv("LUMEN_VULKAN_VALIDATION"); v && *v == '1') {
        if (::access("/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json", R_OK) == 0) {
            g_vk_validation_log = "/tmp/lumen-vk-validation-" + std::to_string(::getpid()) + ".log";
            ::setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
            ::setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG", 1);
            ::setenv("VK_KHRONOS_VALIDATION_LOG_FILENAME", g_vk_validation_log.c_str(), 1);
            ::setenv("VK_KHRONOS_VALIDATION_REPORT_FLAGS", "error,warn", 1);
            std::remove(g_vk_validation_log.c_str());
        } else {
            std::printf("LUMEN_VULKAN_VALIDATION: VK_LAYER_KHRONOS_validation not installed\n");
        }
    }
    const int status = lumen_test::run_all(argc, argv);
    g_vk.reset();
    if (!g_vk_validation_log.empty())
        std::remove(g_vk_validation_log.c_str());
    return status;
}
