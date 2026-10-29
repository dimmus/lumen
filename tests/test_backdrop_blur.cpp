// Backdrop blur (ext-background-effect-v1): the dual-Kawase chain of lx.gfx:renderer
// "Backdrop blur" checked against a reference computed here, in double precision, from that
// definition alone — footprint, level 0 as 2×2 means, the down and up taps, the final
// bilinear sample — with each level stored the way the backend stores it (8-bit on GL,
// 16-bit float in linear light on Vulkan). Plus what decides when it runs: the planner's
// cache and damage, the scene's blur draws, the visibility pass and the backdrop signature.
//
// GL runs on a hardware render node (skipped without one, or on a software GL renderer);
// Vulkan on whatever device the selector finds (lavapipe here). Both read the target back.
#include "lumen_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.scene;

namespace {

constexpr auto k_xrgb = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);

// ── The reference ────────────────────────────────────────────────────────────────────

struct plane {
    int w = 0;
    int h = 0;
    std::vector<double> v; // RGBA, row-major
    double& at(int x, int y, int c) { return v[(static_cast<std::size_t>(y) * w + x) * 4 + c]; }
    double at(int x, int y, int c) const {
        return v[(static_cast<std::size_t>(y) * w + x) * 4 + c];
    }
};

plane make_plane(int w, int h) { return {w, h, std::vector<double>(static_cast<std::size_t>(w) * h * 4)}; }

/// Bilinear at (x, y) in texel units (centres at i + ½), clamped to the texel centres.
double sample(const plane& p, double x, double y, int c) {
    x = std::clamp(x, 0.5, p.w - 0.5);
    y = std::clamp(y, 0.5, p.h - 0.5);
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int x1 = std::min(x0 + 1, p.w - 1);
    const int y1 = std::min(y0 + 1, p.h - 1);
    const double ax = fx - x0;
    const double ay = fy - y0;
    return p.at(x0, y0, c) * (1 - ax) * (1 - ay) + p.at(x1, y0, c) * ax * (1 - ay) +
           p.at(x0, y1, c) * (1 - ax) * ay + p.at(x1, y1, c) * ax * ay;
}

/// How a backend stores a level: 8-bit unorm (GL) or 16-bit float (Vulkan).
enum class storage { unorm8, half };

double store(double v, storage s) {
    if (s == storage::unorm8)
        return std::round(std::clamp(v, 0.0, 1.0) * 255.0) / 255.0;
    return static_cast<double>(static_cast<_Float16>(v));
}

void store_all(plane& p, storage s) {
    for (double& x : p.v)
        x = store(x, s);
}

/// The footprint, from the definition: the box grown by the margin, clipped to the target,
/// origin floored and size floored to even.
lx::rect2i reference_footprint(lx::rect2i box, int width, int height) {
    const int m = lx::gfx::k_blur_margin;
    int x0 = std::max(box.x - m, 0), y0 = std::max(box.y - m, 0);
    const int x1 = std::min(box.x + box.width + m, width);
    const int y1 = std::min(box.y + box.height + m, height);
    x0 -= x0 % 2;
    y0 -= y0 % 2;
    return {x0, y0, (x1 - x0) / 2 * 2, (y1 - y0) / 2 * 2};
}

/// The blurred level 0 of `backdrop` (target-sized, the values a backend blurs) over `f`.
plane reference_blur(const plane& backdrop, lx::rect2i f, storage s) {
    const double r = lx::gfx::k_blur_radius;
    std::vector<plane> levels;
    plane l0 = make_plane(f.width / 2, f.height / 2);
    for (int j = 0; j < l0.h; ++j)
        for (int i = 0; i < l0.w; ++i)
            for (int c = 0; c < 4; ++c) {
                const int x = f.x + 2 * i;
                const int y = f.y + 2 * j;
                l0.at(i, j, c) = (backdrop.at(x, y, c) + backdrop.at(x + 1, y, c) +
                                  backdrop.at(x, y + 1, c) + backdrop.at(x + 1, y + 1, c)) / 4.0;
            }
    store_all(l0, s);
    levels.push_back(std::move(l0));
    for (unsigned k = 1; k < lx::gfx::k_blur_levels; ++k) {
        const plane& src = levels.back();
        plane dst = make_plane((src.w + 1) / 2, (src.h + 1) / 2);
        for (int j = 0; j < dst.h; ++j)
            for (int i = 0; i < dst.w; ++i)
                for (int c = 0; c < 4; ++c) {
                    const double cx = 2.0 * i + 1.0;
                    const double cy = 2.0 * j + 1.0;
                    dst.at(i, j, c) = (4 * sample(src, cx, cy, c) + sample(src, cx - r, cy - r, c) +
                                       sample(src, cx + r, cy + r, c) +
                                       sample(src, cx + r, cy - r, c) +
                                       sample(src, cx - r, cy + r, c)) /
                                      8.0;
                }
        store_all(dst, s);
        levels.push_back(std::move(dst));
    }
    plane up = levels.back();
    for (unsigned k = lx::gfx::k_blur_levels - 1; k > 0; --k) {
        const plane& below = levels[k - 1];
        plane dst = make_plane(below.w, below.h);
        const double h = r / 2;
        for (int j = 0; j < dst.h; ++j)
            for (int i = 0; i < dst.w; ++i)
                for (int c = 0; c < 4; ++c) {
                    const double cx = (i + 0.5) / 2;
                    const double cy = (j + 0.5) / 2;
                    double sum = sample(up, cx - 2 * h, cy, c) + sample(up, cx + 2 * h, cy, c) +
                                 sample(up, cx, cy - 2 * h, c) + sample(up, cx, cy + 2 * h, c);
                    sum += 2 * (sample(up, cx - h, cy - h, c) + sample(up, cx + h, cy + h, c) +
                                sample(up, cx + h, cy - h, c) + sample(up, cx - h, cy + h, c));
                    dst.at(i, j, c) = sum / 12.0;
                }
        store_all(dst, s);
        up = std::move(dst);
    }
    return up;
}

/// The value a blur draw writes at target pixel (x, y): level 0 at the pixel's centre.
double blurred_at(const plane& l0, lx::rect2i f, int x, int y, int c) {
    return sample(l0, (x + 0.5 - f.x) / 2.0, (y + 0.5 - f.y) / 2.0, c);
}

double srgb_decode(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double srgb_encode(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

/// A backdrop with detail at every scale: 8 px checks of two colours over a diagonal ramp.
std::vector<unsigned char> pattern(int w, int h) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            const bool check = ((x / 8) + (y / 8)) % 2 == 0;
            const int ramp = (x + y) % 256;
            p[0] = check ? 240 : static_cast<unsigned char>(ramp);
            p[1] = check ? 40 : static_cast<unsigned char>(255 - ramp);
            p[2] = check ? static_cast<unsigned char>(ramp / 2) : 200;
            p[3] = 255;
        }
    return px;
}

plane to_plane(const std::vector<unsigned char>& rgba, int w, int h, bool linear) {
    plane p = make_plane(w, h);
    for (std::size_t i = 0; i < p.v.size(); ++i) {
        const double e = rgba[i] / 255.0;
        p.v[i] = linear && (i % 4) != 3 ? srgb_decode(e) : e;
    }
    return p;
}

lx::gfx::blit_command texture_draw(unsigned id, lx::rect2i dst) {
    lx::gfx::blit_command cmd{};
    cmd.texture_id = id;
    cmd.dst = dst;
    cmd.blend = lx::blend_mode::opaque;
    return cmd;
}

lx::gfx::blit_command blur_draw(lx::rect2i dst, lx::rect2i box, unsigned source, float opacity = 1.f) {
    lx::gfx::blit_command cmd{};
    cmd.blur = true;
    cmd.dst = dst;
    cmd.blur_box = box;
    cmd.blur_source = source;
    cmd.opacity = opacity;
    cmd.blend = lx::blend_mode::premultiplied;
    return cmd;
}

lx::gfx::blit_command solid_draw(lx::rect2i dst, lx::color c, lx::blend_mode blend) {
    lx::gfx::blit_command cmd{};
    cmd.solid = true;
    cmd.solid_color = c;
    cmd.dst = dst;
    cmd.blend = blend;
    return cmd;
}

struct compare_result {
    int worst = 0;
    unsigned compared = 0;
    unsigned outside_differ = 0;
};

/// GPU pixels inside `region` against `expect(x, y, c)` (0..255, rounded), and pixels outside
/// against `outside` (exact).
template <typename Fn>
compare_result compare(const std::vector<unsigned char>& got, int w, int h, lx::rect2i region,
                       Fn expect, const std::vector<unsigned char>& outside) {
    compare_result r{};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            const bool in = x >= region.x && x < region.x + region.width && y >= region.y &&
                            y < region.y + region.height;
            for (int c = 0; c < 3; ++c) {
                if (in) {
                    const int want = static_cast<int>(std::lround(expect(x, y, c) * 255.0));
                    r.worst = std::max(r.worst, std::abs(static_cast<int>(got[i + c]) - want));
                    ++r.compared;
                } else if (got[i + c] != outside[i + c]) {
                    ++r.outside_differ;
                }
            }
        }
    return r;
}

// ── GL device (as test_gl_composite) ─────────────────────────────────────────────────

lx::gfx::egl_device* gl_device() {
    static lx::gfx::egl_device device{};
    static bool initialized = false;
    static bool usable = false;
    if (!initialized) {
        initialized = true;
        const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0)
            return nullptr;
        auto created = lx::gfx::egl_device::create(fd);
        if (!created) {
            std::printf("  (skip: %s)\n", created.get_error().message);
            return nullptr;
        }
        device = std::move(created).value();
        if (device.is_software_renderer()) {
            std::printf("  (skip: GL renderer is software — %s)\n", device.renderer());
            return nullptr;
        }
        usable = true;
    }
    return usable ? &device : nullptr;
}

struct gl_frame {
    std::vector<unsigned char> rgba;
    bool ok = false;
};

/// One composite of `cmds` into `target`, with a blur frame open over `sources` when given.
gl_frame gl_composite(lx::gfx::gl_compositor& comp, lx::gfx::gl_scanout_target& target,
                      const std::vector<lx::gfx::blit_command>& cmds,
                      std::span<const lx::gfx::blur_source> sources, lx::gfx::damage_region damage,
                      bool* again = nullptr, lx::gfx::damage_region* planned = nullptr) {
    gl_frame f{};
    const int w = static_cast<int>(target.width());
    const int h = static_cast<int>(target.height());
    const bool open = !sources.empty();
    bool replan = false;
    if (open)
        replan = comp.begin_blur_frame(0, target.framebuffer(), sources, damage, w, h);
    if (again)
        *again = replan;
    if (planned)
        *planned = damage;
    auto c = comp.composite_region(target, lx::color::rgb(0.f, 0.f, 0.f), cmds.data(),
                                   static_cast<unsigned>(cmds.size()), damage);
    if (open)
        comp.end_blur_frame();
    if (!c)
        return f;
    if (c.value().out_fence_fd >= 0)
        ::close(c.value().out_fence_fd);
    f.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    f.ok = static_cast<bool>(comp.read_back(target, f.rgba.data(), static_cast<unsigned>(f.rgba.size())));
    return f;
}

lx::gfx::damage_region full_damage() {
    lx::gfx::damage_region d{};
    d.set_full();
    return d;
}

} // namespace

// ── Geometry and planning ────────────────────────────────────────────────────────────

LUMEN_TEST(blur_footprint_follows_the_definition) {
    const lx::rect2i boxes[] = {{96, 40, 128, 72}, {0, 0, 1918, 32}, {201, 121, 120, 79},
                                {-30, -10, 100, 50}, {300, 300, 10, 10}, {5000, 5, 10, 10}};
    for (const lx::rect2i b : boxes) {
        for (const lx::size2i t : {lx::size2i{320, 200}, lx::size2i{321, 201}, lx::size2i{1918, 928}}) {
            const lx::rect2i got = lx::gfx::blur_footprint(b, t.width, t.height);
            lx::rect2i want = reference_footprint(b, t.width, t.height);
            if (want.width < 2 || want.height < 2)
                want = {};
            LUMEN_CHECK(got.x == want.x && got.y == want.y && got.width == want.width &&
                        got.height == want.height);
            if (got.width > 0) {
                LUMEN_CHECK(got.x % 2 == 0 && got.y % 2 == 0 && got.width % 2 == 0 &&
                            got.height % 2 == 0);
                LUMEN_CHECK(got.x + got.width <= t.width && got.y + got.height <= t.height);
            }
        }
    }
    // The levels: half the footprint, then halved rounding up.
    const lx::rect2i f{0, 0, 260, 98};
    LUMEN_CHECK(lx::gfx::blur_level_size(f, 0).width == 130 && lx::gfx::blur_level_size(f, 0).height == 49);
    LUMEN_CHECK(lx::gfx::blur_level_size(f, 1).width == 65 && lx::gfx::blur_level_size(f, 1).height == 25);
    LUMEN_CHECK(lx::gfx::blur_level_size(f, 3).width == 17 && lx::gfx::blur_level_size(f, 3).height == 7);
}

LUMEN_TEST(blur_support_lies_inside_the_margin) {
    // An impulse in level 0: every texel the chain reaches is within the support the
    // margin was chosen for (29 level-0 texels: 60 output pixels with the final sample).
    const int n = 160;
    plane backdrop = make_plane(2 * n, 2 * n);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            for (int c = 0; c < 4; ++c)
                backdrop.at(n + x, n + y, c) = 1.0;
    const plane l0 = reference_blur(backdrop, {0, 0, 2 * n, 2 * n}, storage::half);
    int reach = 0;
    double total = 0;
    for (int j = 0; j < l0.h; ++j)
        for (int i = 0; i < l0.w; ++i) {
            const double v = l0.at(i, j, 0);
            total += v;
            if (v > 0)
                reach = std::max({reach, std::abs(i - n / 2), std::abs(j - n / 2)});
        }
    std::printf("  impulse: reach %d level-0 texels, weight %.6f\n", reach, total);
    LUMEN_CHECK(reach <= 30);
    LUMEN_CHECK(2 * (reach + 1) + 1 < lx::gfx::k_blur_margin);
    LUMEN_CHECK(std::abs(total - 1.0) < 1e-2);
}

LUMEN_TEST(planner_blurs_again_only_when_the_backdrop_changes) {
    lx::gfx::blur_planner plan{};
    const lx::rect2i box{100, 0, 400, 32};
    lx::gfx::blur_source src{box, true, 7};
    const lx::rect2i f = lx::gfx::blur_footprint(box, 1000, 600);

    // New: blurred. The footprint gets a rect of its own, grown over every rect meeting it
    // (and over those meeting the grown rect: {540, 115} misses the footprint, not the rect
    // grown over {30, 90}); a rect far from it stays as it was.
    LUMEN_CHECK(f.x == 36 && f.y == 0 && f.width == 528 && f.height == 96);
    lx::gfx::damage_region d{};
    d.add({30, 90, 20, 30});
    d.add({540, 115, 10, 10});
    d.add({800, 500, 10, 10});
    LUMEN_CHECK(plan.plan({&src, 1}, d, 1000, 600));
    LUMEN_CHECK(plan.pending(0));
    LUMEN_CHECK(d.count == 2);
    const auto holds = [](lx::rect2i a, lx::rect2i b) {
        return b.x >= a.x && b.y >= a.y && b.x + b.width <= a.x + a.width &&
               b.y + b.height <= a.y + a.height;
    };
    const unsigned at = holds(d.rects[0], f) ? 0u : 1u;
    const lx::rect2i grown = d.rects[at];
    const lx::rect2i far = d.rects[1u - at];
    LUMEN_CHECK(holds(grown, f) && holds(grown, {30, 90, 20, 30}) &&
                holds(grown, {540, 115, 10, 10}));
    LUMEN_CHECK(grown.x == 30 && grown.y == 0 && grown.width == 534 && grown.height == 125);
    LUMEN_CHECK(far.x == 800 && far.y == 500 && far.width == 10 && far.height == 10);
    // The buffer age folded in later meets the footprint's rect: `isolate` grows it again.
    lx::gfx::damage_region aged = d;
    aged.add({560, 120, 300, 300});
    aged.add({0, 590, 10, 10});
    plan.isolate(aged);
    LUMEN_CHECK(aged.count == 3);
    bool grown_again = false;
    for (unsigned i = 0; i < aged.count; ++i) {
        const lx::rect2i a = aged.rects[i];
        grown_again = grown_again || (a.x == 30 && a.y == 0 && a.width == 830 && a.height == 420);
        for (unsigned j = i + 1; j < aged.count; ++j) {
            const lx::rect2i b = aged.rects[j];
            LUMEN_CHECK(a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y ||
                        b.y + b.height <= a.y);
        }
    }
    LUMEN_CHECK(grown_again);
    plan.done(0);

    // The surface itself redrawn (damage inside the box), same backdrop: the cached blur.
    lx::gfx::damage_region panel{};
    panel.add({450, 8, 40, 16});
    panel.add({800, 500, 10, 10});
    LUMEN_CHECK(!plan.plan({&src, 1}, panel, 1000, 600));
    LUMEN_CHECK(!plan.pending(0));
    LUMEN_CHECK(panel.count == 2);

    // Something behind it changed: blurred again.
    src.backdrop = 8;
    lx::gfx::damage_region d2{};
    d2.add({300, 60, 10, 10});
    LUMEN_CHECK(plan.plan({&src, 1}, d2, 1000, 600));
    plan.done(0);

    // Moved: blurred again.
    src.box = {102, 0, 400, 32};
    lx::gfx::damage_region d3{};
    d3.add({0, 0, 4, 4});
    LUMEN_CHECK(plan.plan({&src, 1}, d3, 1000, 600));
    plan.done(0);

    // Stale while hidden: nothing grows (it is not drawn), and it stays stale until drawn.
    src.backdrop = 9;
    src.visible = false;
    lx::gfx::damage_region d4{};
    d4.add({0, 0, 4, 4});
    d4.add({900, 500, 4, 4});
    LUMEN_CHECK(!plan.plan({&src, 1}, d4, 1000, 600));
    LUMEN_CHECK(plan.pending(0) && d4.count == 2);
    src.visible = true;
    lx::gfx::damage_region d5{};
    d5.add({0, 0, 4, 4});
    LUMEN_CHECK(plan.plan({&src, 1}, d5, 1000, 600));
}

// ── Scene ────────────────────────────────────────────────────────────────────────────

LUMEN_TEST(surface_blur_region_becomes_blur_draws_under_the_surface) {
    lx::scene::surface_node node{lx::surface_id{5}};
    node.attach(lx::gfx::imported_image{.image_id = 42, .width = 200, .height = 40});
    node.set_bounds({0, 0, 200, 40});
    node.set_transform({100, 50}, 1.f, 0.75f);
    const lx::rect2i region[] = {{0, 0, 120, 40}, {120, 10, 80, 30}};
    node.set_blur_region(region);
    lx::scene::draw_list out{};
    out.begin({1000, 600});
    lx::scene::emit_context ctx{};
    ctx.projection.size = {1000, 600};
    node.emit_draws(out, ctx);
    LUMEN_CHECK(out.size() == 3);
    LUMEN_CHECK(out.blur_source_count() == 1);
    const lx::scene::draw_command* d = out.data();
    LUMEN_CHECK(d[0].blur && d[1].blur && !d[2].blur);
    LUMEN_CHECK(d[0].dst.x == 100 && d[0].dst.y == 50 && d[0].dst.width == 120 && d[0].dst.height == 40);
    LUMEN_CHECK(d[1].dst.x == 220 && d[1].dst.y == 60 && d[1].dst.width == 80 && d[1].dst.height == 30);
    LUMEN_CHECK(d[0].blur_box.x == 100 && d[0].blur_box.width == 200 && d[0].blur_box.height == 40);
    LUMEN_CHECK(d[1].blur_box.width == 200 && d[0].blur_source == 0 && d[1].blur_source == 0);
    LUMEN_CHECK(d[0].opacity == 0.75f && d[0].blend == lx::blend_mode::premultiplied);
    LUMEN_CHECK(d[2].texture.id() == 42);

    // A capture view shows the window, not what is behind it.
    lx::scene::draw_list cap{};
    cap.begin({1000, 600});
    ctx.view = lx::scene::k_capture_view_base;
    node.emit_draws(cap, ctx);
    LUMEN_CHECK(cap.size() == 1 && cap.blur_source_count() == 0);

    // No region: exactly the draw it always was.
    node.set_blur_region({});
    lx::scene::draw_list none{};
    none.begin({1000, 600});
    ctx.view = 0;
    node.emit_draws(none, ctx);
    LUMEN_CHECK(none.size() == 1 && !none.data()[0].blur && none.blur_source_count() == 0);
}

LUMEN_TEST(visibility_pass_keeps_what_a_blur_reads) {
    using lx::scene::draw_command;
    const auto opaque = [](unsigned id, lx::rect2i r) {
        draw_command c{};
        c.texture = lx::texture_id{id};
        c.dst = r;
        c.blend = lx::blend_mode::opaque;
        return c;
    };
    const lx::rect2i panel{0, 0, 400, 30};
    draw_command blur{};
    blur.blur = true;
    blur.dst = panel;
    blur.blur_box = panel;
    blur.blend = lx::blend_mode::premultiplied;
    draw_command panel_surface = opaque(3, panel);
    panel_surface.blend = lx::blend_mode::premultiplied;
    // B lies in the panel's footprint (30 + 64 px down), under C, an opaque window above the
    // panel in paint order.
    std::vector<draw_command> with{opaque(1, {0, 0, 400, 300}), opaque(2, {100, 60, 40, 20}), blur,
                                   panel_surface, opaque(4, {90, 50, 80, 40})};
    std::vector<draw_command> without{with[0], with[1], with[3], with[4]};
    (void)lx::scene::mark_occlusion(without, {0, 0, 400, 300}, nullptr);
    LUMEN_CHECK(without[1].occluded); // control: C hides B
    (void)lx::scene::mark_occlusion(with, {0, 0, 400, 300}, nullptr);
    LUMEN_CHECK(!with[1].occluded); // the blur reads B: drawn
    LUMEN_CHECK(!with[2].occluded); // the blur draw itself shows
    LUMEN_CHECK(!with[0].occluded); // a blur draw never occludes
    // Far below the footprint, C still hides what is under it.
    std::vector<draw_command> far{opaque(1, {0, 0, 400, 300}), opaque(2, {100, 200, 40, 20}), blur,
                                  panel_surface, opaque(4, {90, 190, 80, 40})};
    (void)lx::scene::mark_occlusion(far, {0, 0, 400, 300}, nullptr);
    LUMEN_CHECK(far[1].occluded);
}

LUMEN_TEST(backdrop_signature_follows_what_is_behind_only) {
    lx::scene::surface_node window{lx::surface_id{1}};
    window.attach(lx::gfx::imported_image{.image_id = 10, .width = 300, .height = 200});
    window.set_bounds({0, 0, 300, 200});
    window.set_transform({50, 20});
    lx::scene::surface_node panel{lx::surface_id{2}};
    panel.attach(lx::gfx::imported_image{.image_id = 11, .width = 600, .height = 30});
    panel.set_bounds({0, 0, 600, 30});
    const lx::rect2i all[] = {{0, 0, 600, 30}};
    panel.set_blur_region(all);
    const lx::color clear = lx::color::rgb(0.f, 0.f, 0.f);
    const auto signature = [&]() {
        lx::scene::draw_list out{};
        out.begin({600, 400});
        lx::scene::emit_context ctx{};
        ctx.projection.size = {600, 400};
        window.emit_draws(out, ctx);
        panel.emit_draws(out, ctx);
        const lx::rect2i f = lx::gfx::blur_footprint(out.data()[1].blur_box, 600, 400);
        return lx::scene::blur_backdrop_signature(out, 0, f, clear, {});
    };
    const std::uint64_t a = signature();
    LUMEN_CHECK(signature() == a);
    panel.set_source_damage({0, 0, 10, 10}); // the panel redraws: above the blur
    LUMEN_CHECK(signature() == a);
    window.set_source_damage({0, 0, 10, 10}); // the window under it commits
    const std::uint64_t b = signature();
    LUMEN_CHECK(b != a);
    window.set_transform({50, 300}); // out of the footprint (30 + 64 px)
    const std::uint64_t c = signature();
    window.set_source_damage({0, 0, 10, 10});
    LUMEN_CHECK(signature() == c); // what is out of reach changes nothing
    window.set_transform({52, 20});
    LUMEN_CHECK(signature() != b);
}

// ── GL ───────────────────────────────────────────────────────────────────────────────

namespace {

struct gl_scene {
    int w = 0;
    int h = 0;
    lx::rect2i region{};
    std::vector<unsigned char> backdrop;
};

/// The pixels a GL blur draw of opacity `a` over the pattern should give, in encoded space
/// as GL blends: blurred · a + backdrop · (1 − a).
void check_gl_blur(lx::gfx::gl_compositor& comp, lx::gfx::egl_device& dev, int w, int h,
                   lx::rect2i region, float opacity) {
    auto made = lx::gfx::gl_scanout_target::create(dev, static_cast<unsigned>(w),
                                                   static_cast<unsigned>(h), k_xrgb);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::gl_scanout_target target = std::move(made).value();
    const auto px = pattern(w, h);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, static_cast<unsigned>(w),
                                                   static_cast<unsigned>(h), px.data())));
    const std::vector<lx::gfx::blit_command> plain{texture_draw(1, {0, 0, w, h})};
    const gl_frame base = gl_composite(comp, target, plain, {}, full_damage());
    LUMEN_CHECK(base.ok);

    std::vector<lx::gfx::blit_command> cmds = plain;
    cmds.push_back(blur_draw(region, region, 0, opacity));
    const lx::gfx::blur_source src{region, true, 1};
    const unsigned long long runs = comp.blur_runs();
    const gl_frame got = gl_composite(comp, target, cmds, {&src, 1}, full_damage());
    LUMEN_CHECK(got.ok);
    LUMEN_CHECK(comp.blur_runs() == runs + 1);

    const lx::rect2i f = reference_footprint(region, w, h);
    const plane back = to_plane(px, w, h, false);
    const plane l0 = reference_blur(back, f, storage::unorm8);
    const auto expect = [&](int x, int y, int c) {
        const double b = std::round(blurred_at(l0, f, x, y, c) * 255.0) / 255.0;
        return b * opacity + back.at(x, y, c) * (1.0 - opacity);
    };
    const compare_result r = compare(got.rgba, w, h, region, expect, base.rgba);
    std::printf("  GL %dx%d region %d,%d %dx%d opacity %.2f: %u values, worst %d levels, "
                "outside differing %u\n",
                w, h, region.x, region.y, region.width, region.height, opacity, r.compared,
                r.worst, r.outside_differ);
    LUMEN_CHECK(r.compared > 0);
    LUMEN_CHECK(r.worst <= 2);
    LUMEN_CHECK(r.outside_differ == 0);
    comp.release_blur(0);
}

} // namespace

LUMEN_TEST(gl_blur_matches_the_reference) {
    auto* dev = gl_device();
    if (!dev)
        return;
    LUMEN_CHECK(static_cast<bool>(dev->make_current()));
    lx::gfx::gl_compositor comp{};
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*dev)));
    if (!comp.supports_blur()) {
        std::printf("  (skip: GL has no glBlitFramebuffer — blur not offered)\n");
        return;
    }
    check_gl_blur(comp, *dev, 320, 200, {96, 40, 128, 72}, 1.f);
    check_gl_blur(comp, *dev, 320, 200, {96, 40, 128, 72}, 0.5f);
    // Odd target, the region on its right and bottom edges: the footprint stops one column
    // short of the edge and the last column samples level 0 clamped.
    check_gl_blur(comp, *dev, 321, 201, {201, 121, 120, 80}, 1.f);
    // A panel along the top edge.
    check_gl_blur(comp, *dev, 640, 240, {0, 0, 640, 32}, 1.f);
    comp.shutdown();
}

LUMEN_TEST(gl_blur_follows_the_backdrop_and_touches_nothing_else) {
    auto* dev = gl_device();
    if (!dev)
        return;
    LUMEN_CHECK(static_cast<bool>(dev->make_current()));
    lx::gfx::gl_compositor comp{};
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*dev)));
    if (!comp.supports_blur())
        return;
    const int w = 480, h = 300;
    auto made = lx::gfx::gl_scanout_target::create(*dev, w, h, k_xrgb);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::gl_scanout_target target = std::move(made).value();
    const auto px = pattern(w, h);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, w, h, px.data())));
    const lx::rect2i panel{0, 0, w, 40};
    const lx::rect2i f = reference_footprint(panel, w, h);
    const lx::color red = lx::color::rgb(1.f, 0.f, 0.f);
    const auto frame = [&](lx::rect2i square, lx::color overlay) {
        std::vector<lx::gfx::blit_command> c{texture_draw(1, {0, 0, w, h}),
                                             solid_draw(square, red, lx::blend_mode::opaque),
                                             blur_draw(panel, panel, 0),
                                             solid_draw({300, 8, 60, 24}, overlay,
                                                        lx::blend_mode::premultiplied)};
        return c;
    };
    const lx::color grey = lx::color::rgb(0.25f, 0.25f, 0.25f, 0.5f);
    const lx::color green = lx::color::rgb(0.f, 0.25f, 0.f, 0.5f);

    // Frame 1: everything.
    lx::gfx::blur_source src{panel, true, 100};
    const lx::rect2i sq1{40, 70, 24, 24}; // under the footprint (40 + 64), not the panel
    const gl_frame one = gl_composite(comp, target, frame(sq1, grey), {&src, 1}, full_damage());
    LUMEN_CHECK(one.ok);
    const unsigned long long runs1 = comp.blur_runs();

    // Frame 2: the square moves (its old and new rects damaged, the backdrop's signature
    // changes): the panel is blurred again, in a rect grown over the footprint and the
    // square; the far rect stays a rect of its own.
    const lx::rect2i sq2{70, 70, 24, 24};
    lx::gfx::damage_region d{};
    d.add(sq1);
    d.add(sq2);
    d.add({400, 250, 10, 10}); // and something far away
    src.backdrop = 101;
    bool again = false;
    const gl_frame two = gl_composite(comp, target, frame(sq2, grey), {&src, 1}, d, &again, &d);
    LUMEN_CHECK(two.ok && again);
    LUMEN_CHECK(comp.blur_runs() == runs1 + 1);
    LUMEN_CHECK(d.count == 2);
    const unsigned at = d.rects[0].width > 10 ? 0u : 1u;
    const lx::rect2i grown = d.rects[at];
    const lx::rect2i far = d.rects[1u - at];
    LUMEN_CHECK(grown.x <= f.x && grown.y <= f.y && grown.x + grown.width >= f.x + f.width &&
                grown.y + grown.height >= f.y + f.height);
    LUMEN_CHECK(far.x == 400 && far.y == 250 && far.width == 10 && far.height == 10);
    // The panel shows the new backdrop blurred (the overlay on top, as before)...
    plane back = to_plane(px, w, h, false);
    for (int y = sq2.y; y < sq2.y + sq2.height; ++y)
        for (int x = sq2.x; x < sq2.x + sq2.width; ++x) {
            back.at(x, y, 0) = 1.0;
            back.at(x, y, 1) = 0.0;
            back.at(x, y, 2) = 0.0;
        }
    const plane l0 = reference_blur(back, f, storage::unorm8);
    const lx::rect2i bare{0, 0, w, 8}; // the panel's rows above the overlay
    const compare_result r = compare(
        two.rgba, w, h, bare,
        [&](int x, int y, int c) { return blurred_at(l0, f, x, y, c); }, two.rgba);
    std::printf("  GL moved backdrop: panel rows worst %d levels over %u values\n", r.worst,
                r.compared);
    LUMEN_CHECK(r.worst <= 2);
    // ...and it changed where the square's blur reaches.
    unsigned changed_in_panel = 0;
    unsigned changed_outside = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            const bool differ = std::memcmp(&one.rgba[i], &two.rgba[i], 3) != 0;
            const bool in_panel = y < panel.height;
            const bool in_square = (x >= sq1.x && x < sq2.x + sq2.width && y >= sq1.y &&
                                    y < sq1.y + sq1.height);
            if (differ && in_panel)
                ++changed_in_panel;
            if (differ && !in_panel && !in_square)
                ++changed_outside;
        }
    std::printf("  GL moved backdrop: %u panel pixels changed, %u elsewhere\n", changed_in_panel,
                changed_outside);
    LUMEN_CHECK(changed_in_panel > 0);
    LUMEN_CHECK(changed_outside == 0);

    // Frame 3: only the panel's own content changes (its overlay turns green, same backdrop):
    // the cached blur is drawn, nothing is blurred, nothing outside the overlay changes.
    lx::gfx::damage_region d3{};
    d3.add({300, 8, 60, 24});
    d3.add({400, 250, 10, 10});
    const gl_frame three =
        gl_composite(comp, target, frame(sq2, green), {&src, 1}, d3, &again, &d3);
    LUMEN_CHECK(three.ok && !again);
    LUMEN_CHECK(comp.blur_runs() == runs1 + 1);
    LUMEN_CHECK(d3.count == 2);
    unsigned outside_overlay = 0;
    unsigned inside_overlay = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            const bool differ = std::memcmp(&two.rgba[i], &three.rgba[i], 3) != 0;
            const bool in = x >= 300 && x < 360 && y >= 8 && y < 32;
            (in ? inside_overlay : outside_overlay) += differ ? 1u : 0u;
        }
    std::printf("  GL panel-only update: %u overlay pixels changed, %u elsewhere, blurs %llu\n",
                inside_overlay, outside_overlay, comp.blur_runs() - runs1);
    LUMEN_CHECK(inside_overlay > 0 && outside_overlay == 0);
    // The cached blur under the green overlay is still the reference: overlay over it.
    const compare_result r3 = compare(
        three.rgba, w, h, {300, 8, 60, 24},
        [&](int x, int y, int c) {
            const double b = std::round(blurred_at(l0, f, x, y, c) * 255.0) / 255.0;
            const double o = c == 1 ? 0.25 : 0.0;
            return o + b * 0.5;
        },
        three.rgba);
    std::printf("  GL cached blur under the overlay: worst %d levels\n", r3.worst);
    LUMEN_CHECK(r3.worst <= 2);

    // Frame 4: stale again, composited (against the plan) in a pass that holds only part of
    // the footprint: the blur would read pixels this pass does not redraw, so it is culled
    // and the region stays stale; the next frame, planned, blurs it.
    src.backdrop = 102;
    lx::gfx::damage_region d4{};
    d4.add({0, 0, w, 10});
    LUMEN_CHECK(comp.begin_blur_frame(0, target.framebuffer(), {&src, 1}, d4, w, h));
    lx::gfx::damage_region part{};
    part.add({0, 0, w, 10});
    const auto four = frame(sq2, green);
    auto c4 = comp.composite_region(target, lx::color::rgb(0.f, 0.f, 0.f), four.data(),
                                    static_cast<unsigned>(four.size()), part);
    comp.end_blur_frame();
    LUMEN_CHECK(static_cast<bool>(c4));
    if (c4 && c4.value().out_fence_fd >= 0)
        ::close(c4.value().out_fence_fd);
    LUMEN_CHECK(comp.blur_runs() == runs1 + 1);
    lx::gfx::damage_region d5{};
    d5.add({0, 0, w, 10});
    const gl_frame five = gl_composite(comp, target, frame(sq2, green), {&src, 1}, d5, &again);
    LUMEN_CHECK(five.ok && again);
    LUMEN_CHECK(comp.blur_runs() == runs1 + 2);
    comp.release_blur(0);
    LUMEN_CHECK(!comp.blur_in_use(0));
    comp.shutdown();
}

LUMEN_TEST(gl_blur_draws_nothing_without_a_blur_frame) {
    auto* dev = gl_device();
    if (!dev)
        return;
    LUMEN_CHECK(static_cast<bool>(dev->make_current()));
    lx::gfx::gl_compositor comp{};
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*dev)));
    const int w = 160, h = 120;
    auto made = lx::gfx::gl_scanout_target::create(*dev, w, h, k_xrgb);
    if (!made)
        return;
    lx::gfx::gl_scanout_target target = std::move(made).value();
    const auto px = pattern(w, h);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, w, h, px.data())));
    const std::vector<lx::gfx::blit_command> plain{texture_draw(1, {0, 0, w, h})};
    std::vector<lx::gfx::blit_command> with = plain;
    with.push_back(blur_draw({10, 10, 60, 40}, {10, 10, 60, 40}, 0));
    const gl_frame a = gl_composite(comp, target, plain, {}, full_damage());
    // No blur frame open (another target, a capture): the blur draw is skipped.
    const gl_frame b = gl_composite(comp, target, with, {}, full_damage());
    // Faded out entirely: nothing either.
    std::vector<lx::gfx::blit_command> faded = plain;
    faded.push_back(blur_draw({10, 10, 60, 40}, {10, 10, 60, 40}, 0, 0.f));
    const lx::gfx::blur_source src{{10, 10, 60, 40}, true, 1};
    const gl_frame c = gl_composite(comp, target, faded, {&src, 1}, full_damage());
    LUMEN_CHECK(a.ok && b.ok && c.ok);
    LUMEN_CHECK(a.rgba == b.rgba);
    LUMEN_CHECK(a.rgba == c.rgba);
    comp.release_blur(0);
    comp.shutdown();
}

// ── Vulkan ───────────────────────────────────────────────────────────────────────────

namespace {

struct vk_fixture {
    lx::gfx::device device{};
    lx::gfx::vulkan_compositor compositor{};
    bool usable = false;
};

/// Torn down by `main`: the loader and the validation layer are gone by static destruction.
std::unique_ptr<vk_fixture> g_vk{};
std::string g_vk_validation_log{};

vk_fixture* vk() {
    static bool tried = false;
    if (!tried) {
        tried = true;
        auto selected = lx::gfx::device_selector::select_best();
        if (!selected || !selected.value().info().supports_dmabuf_import) {
            std::printf("  (skip: no Vulkan device)\n");
            return nullptr;
        }
        auto f = std::make_unique<vk_fixture>();
        f->device = std::move(selected).value();
        if (auto ready = f->compositor.initialize(f->device.context()); !ready) {
            std::printf("  (skip: %s)\n", ready.get_error().message);
            return nullptr;
        }
        f->usable = true;
        g_vk = std::move(f);
    }
    return g_vk.get();
}

std::vector<unsigned char> vk_composite(vk_fixture& v, lx::gfx::render_target& target,
                                        const std::vector<lx::gfx::blit_command>& cmds,
                                        std::span<const lx::gfx::blur_source> sources,
                                        lx::gfx::damage_region damage) {
    const int w = static_cast<int>(target.width());
    const int h = static_cast<int>(target.height());
    if (!sources.empty())
        (void)v.compositor.begin_blur_frame(0, sources, damage, w, h);
    auto c = v.compositor.composite(target, lx::color::rgb(0.f, 0.f, 0.f), cmds.data(),
                                    static_cast<unsigned>(cmds.size()));
    if (!sources.empty())
        v.compositor.end_blur_frame();
    std::vector<unsigned char> out;
    if (!c)
        return out;
    if (c.value().out_fence_fd >= 0)
        ::close(c.value().out_fence_fd);
    out.resize(static_cast<std::size_t>(w) * h * 4);
    if (!v.compositor.read_back(target, out.data(), static_cast<unsigned>(out.size())))
        out.clear();
    return out;
}

} // namespace

LUMEN_TEST(vulkan_blur_matches_the_reference) {
    vk_fixture* v = vk();
    if (!v)
        return;
    for (const auto& [w, h, region] :
         {std::tuple{320, 200, lx::rect2i{96, 40, 128, 72}},
          std::tuple{321, 201, lx::rect2i{97, 41, 128, 72}},
          std::tuple{400, 300, lx::rect2i{200, 120, 120, 80}},
          std::tuple{321, 201, lx::rect2i{201, 121, 120, 80}}}) {
        auto made = lx::gfx::render_target::create(v->device.context(), static_cast<unsigned>(w),
                                                   static_cast<unsigned>(h));
        LUMEN_CHECK(static_cast<bool>(made));
        if (!made)
            return;
        lx::gfx::render_target target = std::move(made).value();
        const auto px = pattern(w, h);
        LUMEN_CHECK(static_cast<bool>(v->compositor.upload_rgba(1, static_cast<unsigned>(w),
                                                                static_cast<unsigned>(h),
                                                                px.data())));
        const std::vector<lx::gfx::blit_command> plain{texture_draw(1, {0, 0, w, h})};
        const auto base = vk_composite(*v, target, plain, {}, full_damage());
        std::vector<lx::gfx::blit_command> cmds = plain;
        cmds.push_back(blur_draw(region, region, 0));
        const lx::gfx::blur_source src{region, true, 1};
        const unsigned long long runs = v->compositor.blur_runs();
        const auto got = vk_composite(*v, target, cmds, {&src, 1}, full_damage());
        LUMEN_CHECK(!base.empty() && !got.empty());
        if (base.empty() || got.empty())
            return;
        LUMEN_CHECK(v->compositor.blur_runs() == runs + 1);
        // Vulkan blurs linear light at 16-bit float (the intermediate it reads is that).
        const lx::rect2i f = reference_footprint(region, w, h);
        plane back = to_plane(px, w, h, true);
        store_all(back, storage::half);
        const plane l0 = reference_blur(back, f, storage::half);
        const compare_result r = compare(
            got, w, h, region,
            [&](int x, int y, int c) {
                return srgb_encode(static_cast<double>(
                    static_cast<_Float16>(blurred_at(l0, f, x, y, c))));
            },
            base);
        std::printf("  Vulkan %dx%d region %d,%d %dx%d: %u values, worst %d levels, outside "
                    "differing %u\n",
                    w, h, region.x, region.y, region.width, region.height, r.compared, r.worst,
                    r.outside_differ);
        LUMEN_CHECK(r.compared > 0);
        LUMEN_CHECK(r.worst <= 2);
        LUMEN_CHECK(r.outside_differ == 0);

        // The same frame again with the same backdrop: drawn from the cache, the same pixels.
        const auto cached = vk_composite(*v, target, cmds, {&src, 1}, full_damage());
        LUMEN_CHECK(v->compositor.blur_runs() == runs + 1);
        LUMEN_CHECK(cached == got);
        v->compositor.release_blur(0);
        LUMEN_CHECK(!v->compositor.blur_in_use(0));
        v->compositor.forget_texture(1);
    }
}

// ── The primitive stream (server-side decorations' frames) ────────────────────────────

namespace {

/// A stream frame as the compositor's scene makes one: the backdrop texture, a red quad
/// primitive inside the footprint (it starts the stream), the blur draw, a quad above it.
struct stream_frame {
    lx::scene::draw_list list{};
    std::vector<lx::gfx::blit_command> blits;
    lx::gfx::ui_stream stream{};
};

lx::gfx::quad_instance solid_quad(lx::rect2i r, lx::color c) {
    lx::gfx::quad_instance q{};
    q.bounds[0] = static_cast<float>(r.x);
    q.bounds[1] = static_cast<float>(r.y);
    q.bounds[2] = static_cast<float>(r.width);
    q.bounds[3] = static_cast<float>(r.height);
    const lx::gfx::packed_rgba bg = lx::gfx::premultiplied_half(c);
    q.background[0] = q.background_end[0] = bg.r;
    q.background[1] = q.background_end[1] = bg.g;
    q.background[2] = q.background_end[2] = bg.b;
    q.background[3] = q.background_end[3] = bg.a;
    return q;
}

void build_stream(stream_frame& f, int w, int h, lx::rect2i square, lx::rect2i region) {
    lx::scene::draw_list& l = f.list;
    l.begin({w, h});
    lx::scene::draw_command back{};
    back.texture = lx::texture_id{1};
    back.dst = {0, 0, w, h};
    back.blend = lx::blend_mode::opaque;
    back.sort_key = lx::draw_sort_key::make(0, l.size());
    l.push(back);
    l.push_quad(solid_quad(square, lx::color::rgb(1.f, 0.f, 0.f)), square,
                l.orders().assign(square));
    lx::scene::draw_command blur{};
    blur.blur = true;
    blur.dst = region;
    blur.blur_box = region;
    blur.blur_source = l.next_blur_source();
    blur.blend = lx::blend_mode::premultiplied;
    blur.sort_key = lx::draw_sort_key::make(0, l.size());
    l.push(blur);
    // Above the region and outside it, but inside its footprint: not part of the backdrop.
    const lx::rect2i above{square.x + square.width + 8, square.y, 20, 20};
    l.push_quad(solid_quad(above, lx::color::rgb(0.f, 0.f, 1.f)), above, l.orders().assign(above));
    l.batch_for_render();
    f.blits.clear();
    for (unsigned i = 0; i < l.size(); ++i) {
        const lx::scene::draw_command& d = l.data()[i];
        lx::gfx::blit_command b{};
        b.texture_id = d.texture.id();
        b.dst = d.dst;
        b.blend = d.blend;
        b.opacity = d.opacity;
        b.blur = d.blur;
        b.blur_box = d.blur_box;
        b.blur_source = d.blur_source;
        f.blits.push_back(b);
    }
    f.stream.refs = l.refs();
    f.stream.batches = l.batches();
    f.stream.quads = l.quad_instances();
    f.stream.shadows = l.shadow_instances();
    f.stream.sprites = l.sprite_instances();
    f.stream.underlines = l.underline_instances();
    f.stream.surfaces = {f.blits.data(), f.blits.size()};
}

/// The backdrop the stream's blur reads: the pattern with the red square on it (the blue
/// quad is above the blur draw).
plane stream_backdrop(const std::vector<unsigned char>& px, int w, int h, lx::rect2i square,
                      bool linear) {
    plane p = to_plane(px, w, h, linear);
    for (int y = square.y; y < square.y + square.height; ++y)
        for (int x = square.x; x < square.x + square.width; ++x) {
            p.at(x, y, 0) = 1.0;
            p.at(x, y, 1) = 0.0;
            p.at(x, y, 2) = 0.0;
        }
    return p;
}

} // namespace

LUMEN_TEST(gl_stream_frame_blurs_primitives_under_the_region) {
    auto* dev = gl_device();
    if (!dev || !dev->gles_at_least(3, 1))
        return;
    LUMEN_CHECK(static_cast<bool>(dev->make_current()));
    lx::gfx::gl_compositor comp{};
    lx::gfx::ui_gl_renderer ui{};
    LUMEN_CHECK(static_cast<bool>(comp.initialize(*dev)));
    LUMEN_CHECK(static_cast<bool>(ui.initialize()));
    if (!comp.supports_blur())
        return;
    ui.set_surface_compositor(&comp);
    const int w = 320, h = 200;
    auto made = lx::gfx::ui_gl_target::create(w, h);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::ui_gl_target target = std::move(made).value();
    const auto px = pattern(w, h);
    LUMEN_CHECK(static_cast<bool>(comp.upload_rgba(1, w, h, px.data())));
    const lx::rect2i region{96, 40, 128, 40};
    const lx::rect2i square{110, 100, 24, 24}; // under the footprint, below the region
    stream_frame f{};
    build_stream(f, w, h, square, region);
    LUMEN_CHECK(f.list.primitive_count() == 2 && !f.stream.batches.empty());
    lx::gfx::damage_region d = full_damage();
    const lx::gfx::blur_source src{region, true, 1};
    LUMEN_CHECK(comp.begin_blur_frame(0, target.framebuffer(), {&src, 1}, d, w, h));
    auto drawn = ui.draw(target.framebuffer(), w, h, f.stream, nullptr, d, lx::color::rgb(0.f, 0.f, 0.f));
    LUMEN_CHECK(static_cast<bool>(drawn));
    auto fence = comp.fence_frame();
    if (fence && fence.value() >= 0)
        ::close(fence.value());
    comp.end_blur_frame();
    LUMEN_CHECK(comp.blur_runs() == 1);
    std::vector<std::uint32_t> words(static_cast<std::size_t>(w) * h);
    LUMEN_CHECK(static_cast<bool>(target.read_back(words)));
    std::vector<unsigned char> got(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        got[i * 4] = static_cast<unsigned char>(words[i] >> 16);
        got[i * 4 + 1] = static_cast<unsigned char>(words[i] >> 8);
        got[i * 4 + 2] = static_cast<unsigned char>(words[i]);
        got[i * 4 + 3] = 255;
    }
    const lx::rect2i fp = reference_footprint(region, w, h);
    const plane back = stream_backdrop(px, w, h, square, false);
    const plane l0 = reference_blur(back, fp, storage::unorm8);
    const compare_result r = compare(
        got, w, h, region, [&](int x, int y, int c) { return blurred_at(l0, fp, x, y, c); }, got);
    std::printf("  GL stream: worst %d levels over %u values\n", r.worst, r.compared);
    LUMEN_CHECK(r.worst <= 2);
    comp.release_blur(0);
    ui.shutdown();
    comp.shutdown();
}

LUMEN_TEST(vulkan_stream_frame_blurs_primitives_under_the_region) {
    vk_fixture* v = vk();
    if (!v)
        return;
    lx::gfx::ui_vk_renderer ui{};
    if (auto ok = ui.initialize(v->device.context()); !ok) {
        std::printf("  (skip: %s)\n", ok.get_error().message);
        return;
    }
    const int w = 320, h = 200;
    auto made = lx::gfx::render_target::create(v->device.context(), w, h);
    LUMEN_CHECK(static_cast<bool>(made));
    if (!made)
        return;
    lx::gfx::render_target target = std::move(made).value();
    const auto px = pattern(w, h);
    LUMEN_CHECK(static_cast<bool>(v->compositor.upload_rgba(1, w, h, px.data())));
    const lx::rect2i region{96, 40, 128, 40};
    const lx::rect2i square{110, 100, 24, 24};
    stream_frame f{};
    build_stream(f, w, h, square, region);
    lx::gfx::damage_region d = full_damage();
    const lx::gfx::blur_source src{region, true, 1};
    const unsigned long long runs = v->compositor.blur_runs();
    LUMEN_CHECK(v->compositor.begin_blur_frame(0, {&src, 1}, d, w, h));
    auto c = ui.composite(v->compositor, target, lx::color::rgb(0.f, 0.f, 0.f), f.stream, nullptr);
    v->compositor.end_blur_frame();
    LUMEN_CHECK(static_cast<bool>(c));
    if (!c)
        return;
    if (c->composite.out_fence_fd >= 0)
        ::close(c->composite.out_fence_fd);
    LUMEN_CHECK(v->compositor.blur_runs() == runs + 1);
    std::vector<unsigned char> got(static_cast<std::size_t>(w) * h * 4);
    LUMEN_CHECK(static_cast<bool>(
        v->compositor.read_back(target, got.data(), static_cast<unsigned>(got.size()))));
    const lx::rect2i fp = reference_footprint(region, w, h);
    plane back = stream_backdrop(px, w, h, square, true);
    store_all(back, storage::half);
    const plane l0 = reference_blur(back, fp, storage::half);
    const compare_result r = compare(
        got, w, h, region,
        [&](int x, int y, int ch) {
            return srgb_encode(static_cast<double>(static_cast<_Float16>(blurred_at(l0, fp, x, y, ch))));
        },
        got);
    std::printf("  Vulkan stream: worst %d levels over %u values\n", r.worst, r.compared);
    LUMEN_CHECK(r.worst <= 2);
    v->compositor.release_blur(0);
    v->compositor.forget_texture(1);
    ui.shutdown();
}

// Last: under LUMEN_VULKAN_VALIDATION=1 (test_backdrop_blur_vk_validation) every Vulkan case
// above ran with VK_LAYER_KHRONOS_validation, which must have logged no error or warning.
LUMEN_TEST(vulkan_validation_clean) {
    if (g_vk_validation_log.empty())
        return;
    std::FILE* f = std::fopen(g_vk_validation_log.c_str(), "r");
    unsigned lines = 0;
    char line[2048];
    while (f && std::fgets(line, sizeof line, f)) {
        if (lines < 5)
            std::printf("    %s", line);
        ++lines;
    }
    if (f)
        std::fclose(f);
    std::printf("    validation layer log: %u lines\n", lines);
    LUMEN_CHECK(lines == 0);
}

int main(int argc, char** argv) {
    if (const char* v = std::getenv("LUMEN_VULKAN_VALIDATION"); v && *v == '1') {
        const char* found = std::getenv("LUMEN_VK_VALIDATION_LAYER");
        const std::string manifest =
            found && *found ? found
                            : "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json";
        if (::access(manifest.c_str(), R_OK) == 0) {
            g_vk_validation_log =
                "/tmp/lumen-blur-vk-validation-" + std::to_string(::getpid()) + ".log";
            ::setenv("VK_ADD_LAYER_PATH", manifest.substr(0, manifest.find_last_of('/')).c_str(), 1);
            ::setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
            ::setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG", 1);
            ::setenv("VK_KHRONOS_VALIDATION_LOG_FILENAME", g_vk_validation_log.c_str(), 1);
            ::setenv("VK_KHRONOS_VALIDATION_REPORT_FLAGS", "error,warn", 1);
            std::remove(g_vk_validation_log.c_str());
        }
    }
    const int status = lumen_test::run_all(argc, argv);
    g_vk.reset();
    if (!g_vk_validation_log.empty())
        std::remove(g_vk_validation_log.c_str());
    return status;
}
