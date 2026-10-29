// Server-side title bars as atlas glyphs on the three compositors (ADR-010 §4.11, Phase 2):
//   - the CPU compositor draws decorated windows — title text and close glyph as mono
//     sprites from the compositor's text atlas — exactly as the committed references
//     (tests/golden/compositor/*.png; LUMEN_UI_GOLDEN_UPDATE=1 rewrites them), at scale 1,
//     at 1.25 (1:1 there, not resampled) and on a turned output;
//   - the GL compositor matches it within G10's 3/255 (both blend UI in the encoded space);
//   - the Vulkan compositor records the stream inside its linear F16 pass: fills, borders
//     and window content within 2/255 of the CPU, glyph edges blended in linear light with
//     the `text.contrast` exponent — their mean stem weight is held to the CPU's within
//     3 % and every pixel within 16/255 (measured below; see `vk_titles_in_the_linear_pass`);
//   - a dimmed (not-responding) frame's fill and glyph colours agree on every backend;
//   - a Vulkan frame without primitives is the blit composite byte for byte; the
//     compositor waits for a UI renderer's frames before it drops a texture they sampled;
//   - screen capture's re-composite and the headless dump draw the titles too;
//   - a steady frame (emit, order, batch, paint) allocates nothing.
// Lavapipe: correctness only. LUMEN_VULKAN_VALIDATION=1 runs the Vulkan cases under
// VK_LAYER_KHRONOS_validation and `vk_validation_clean` (last) requires a clean log.
#include <fcntl.h>
#include <png.h>
#include <stdlib.h>
#include <unistd.h>

#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.compositor;

// ── Allocation counter (the steady-frame check) ─────────────────────────────────────

namespace {
std::atomic<unsigned long long> g_allocations{0};
} // namespace

void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using lx::compositor::deco_button;

constexpr int k_w = 320;
constexpr int k_h = 176;
constexpr lx::color k_black = lx::color::rgb(0.f, 0.f, 0.f);
/// The decoration atlas (circles, the fallback close glyph) counts as staged.
bool g_atlas_ready = true;

/// `cpu_compositor`'s dim of an encoded byte (0.65 as 166/256), per channel of `rgb`.
[[nodiscard]] constexpr std::uint32_t dimmed(std::uint32_t rgb) {
    std::uint32_t out = 0;
    for (unsigned sh = 0; sh < 24; sh += 8)
        out |= ((((rgb >> sh) & 0xFFu) * 166u) >> 8u) << sh;
    return out;
}

/// The decoration atlas (button circles, the close glyph's fallback), premultiplied ARGB.
const std::vector<std::uint32_t>& deco_atlas() {
    static const std::vector<std::uint32_t> a = [] {
        std::vector<std::uint32_t> px(lx::compositor::k_deco_atlas_size * lx::compositor::k_deco_atlas_size);
        lx::compositor::build_deco_atlas(px.data());
        return px;
    }();
    return a;
}

/// RGBA bytes of premultiplied ARGB words (what the GL and Vulkan compositors upload).
std::vector<unsigned char> rgba_of(const std::vector<std::uint32_t>& words) {
    std::vector<unsigned char> out(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        out[i * 4 + 0] = static_cast<unsigned char>(words[i] >> 16);
        out[i * 4 + 1] = static_cast<unsigned char>(words[i] >> 8);
        out[i * 4 + 2] = static_cast<unsigned char>(words[i]);
        out[i * 4 + 3] = static_cast<unsigned char>(words[i] >> 24);
    }
    return out;
}

/// A layout that succeeded and changed the run.
template <typename R>
[[nodiscard]] bool laid(const R& r) {
    return r && r.value();
}
/// A layout that succeeded and left the run as it was.
template <typename R>
[[nodiscard]] bool kept(const R& r) {
    return r && !r.value();
}

struct titled_window {
    lx::rect2i geometry;
    const char* title;
    bool active;
    bool not_responding;
    deco_button button;
    lx::color content;
};

// Active and fitting; inactive, hovered and ellipsized; not responding (dimmed, the suffix
// alone and itself cut) — every rule of the title, at scale 1.
const titled_window k_windows[3] = {
    {{12, 40, 296, 36}, "Terminal \xE2\x80\x94 ~/src/lumen (main)", true, false, deco_button::normal,
     lx::color::rgb(0.10f, 0.12f, 0.15f)},
    {{12, 116, 150, 44}, "A very long document title that cannot fit in its bar", false, false,
     deco_button::hover, lx::color::rgb(0.85f, 0.85f, 0.82f)},
    {{176, 116, 132, 44}, "Editor", false, true, deco_button::normal,
     lx::color::rgb(0.20f, 0.30f, 0.45f)},
};
// The same at scale 1.25 (logical pixels: 256 × 141 of them on the 320 × 176 output).
const titled_window k_windows_125[2] = {
    {{10, 32, 236, 28}, "Terminal \xE2\x80\x94 ~/src/lumen (main)", true, false, deco_button::pressed,
     lx::color::rgb(0.10f, 0.12f, 0.15f)},
    {{10, 93, 236, 36}, "Files", false, true, deco_button::normal, lx::color::rgb(0.85f, 0.85f, 0.82f)},
};

/// The text atlas and the title font every case uses: the bundled UI family, bold, 10 pt —
/// deterministic on any host (the compositor's default "Sans" is whatever fontconfig picks).
struct text_env {
    lx::compositor::chrome_atlas atlas{};
    lx::compositor::chrome_text text{atlas};
    text_env() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        text.set_font({lx::text::k_ui_family, 10.f, lx::text::weight::bold, lx::text::style::normal});
    }
};

text_env& env() {
    static text_env e{};
    return e;
}

/// One output view of decorated windows: each a frame node (title laid out at the view's
/// scale, close glyph rasterised for it) under a solid, opaque window.
struct desktop {
    std::unique_ptr<lx::scene::scene_graph> scene = std::make_unique<lx::scene::scene_graph>();
    std::vector<std::unique_ptr<lx::compositor::frame_node>> frames;
    std::vector<std::unique_ptr<lx::compositor::title_text>> titles;
    std::vector<std::unique_ptr<lx::scene::surface_node>> windows;
    unsigned frame_index = 0;

    desktop(std::span<const titled_window> ws, double scale = 1.0,
            lx::buffer_transform t = lx::buffer_transform::normal, bool decorated = true,
            double title_scale = 0.0) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        text_env& e = env();
        const bool turned = (static_cast<unsigned>(t) & 1u) != 0;
        const int cw = turned ? k_h : k_w;
        const int ch = turned ? k_w : k_h;
        lx::scene::output_view_config v{};
        v.logical = {0, 0, static_cast<int>(std::lround(cw / scale)),
                     static_cast<int>(std::lround(ch / scale))};
        v.projection = {{0, 0}, scale, {k_w, k_h}, t};
        scene->set_output_view(0, v);
        const double ts = title_scale > 0.0 ? title_scale : scale;
        unsigned id = 10;
        for (const titled_window& w : ws) {
            if (decorated) {
                auto tt = std::make_unique<lx::compositor::title_text>();
                auto laid = e.text.layout_title(tt->runs[0], w.title, w.not_responding,
                                                lx::compositor::title_text_room(w.geometry.width),
                                                ts, true);
                LUMEN_CHECK(laid && laid.value());
                LUMEN_CHECK(e.text.close(ts) != nullptr);
                (void)e.text.close(scale);
                auto f = std::make_unique<lx::compositor::frame_node>();
                f->geometry = w.geometry;
                f->extents = lx::compositor::extents_for(lx::compositor::decoration_mode::server, false);
                f->shown = true;
                f->active = w.active;
                f->button = w.button;
                f->dimmed = w.not_responding;
                f->atlas_ready = &g_atlas_ready;
                f->title = tt.get();
                f->text = &e.text;
                (void)scene->root().add(f.get());
                frames.push_back(std::move(f));
                titles.push_back(std::move(tt));
            }
            auto s = std::make_unique<lx::scene::surface_node>(lx::surface_id{id++});
            s->set_bounds({0, 0, w.geometry.width, w.geometry.height});
            s->set_transform({w.geometry.x, w.geometry.y});
            s->set_solid(true, w.content);
            s->set_opaque(true);
            (void)scene->root().add(s.get());
            windows.push_back(std::move(s));
        }
    }

    const lx::scene::immutable_frame_snapshot& publish() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        scene->note_full_damage_output(0);
        LUMEN_CHECK(scene->commit_frame(0, ++frame_index));
        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        return scene->acquire_render_snapshot(0);
    }
    void release(const lx::scene::immutable_frame_snapshot& s) {
        scene->release_render_frame(0, s);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    }
};

lx::gfx::damage_region full_region() {
    lx::gfx::damage_region r{};
    r.full = true;
    return r;
}

lx::gfx::pixel_surface surface_of(std::vector<std::uint32_t>& px, int w, int h) {
    lx::gfx::pixel_surface s{};
    s.pixels = reinterpret_cast<unsigned char*>(px.data());
    s.width = static_cast<unsigned>(w);
    s.height = static_cast<unsigned>(h);
    s.stride = static_cast<unsigned>(w) * 4u;
    s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    return s;
}

/// The CPU compositor's frame of `d` (XRGB words).
lx::gfx::cpu_compositor& cpu_comp() {
    static lx::gfx::cpu_compositor comp;
    static bool registered = false;
    if (!registered) {
        registered = true;
        lx::gfx::pixel_surface a{};
        a.pixels = reinterpret_cast<unsigned char*>(const_cast<std::uint32_t*>(deco_atlas().data()));
        a.width = a.height = lx::compositor::k_deco_atlas_size;
        a.stride = lx::compositor::k_deco_atlas_size * 4u;
        a.format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
        LUMEN_CHECK(static_cast<bool>(comp.register_texture(lx::compositor::k_deco_atlas_texture_id, a)));
    }
    return comp;
}

std::vector<std::uint32_t> paint_cpu(desktop& d) {
    lx::gfx::cpu_compositor& comp = cpu_comp();
    static lx::gfx::ui_cpu_renderer ui;
    static lx::compositor::frame_painter painter;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(k_w * k_h), 0u);
    const auto& snap = d.publish();
    auto p = painter.paint_cpu(comp, &ui, surface_of(px, k_w, k_h), snap, full_region(), &env().atlas);
    LUMEN_CHECK(p && p->paint.stream && p->ui.primitives_skipped == 0 &&
                p->composite.draws_skipped == 0);
    d.release(snap);
    return px;
}

[[nodiscard]] int channel(std::uint32_t p, unsigned shift) { return static_cast<int>((p >> shift) & 0xFFu); }

/// Max channel difference over the colour bytes, optionally only where `where(x, y)`.
template <typename Where>
int max_diff(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, Where where,
             int* wx = nullptr, int* wy = nullptr) {
    int worst = 0;
    for (int y = 0; y < k_h; ++y)
        for (int x = 0; x < k_w; ++x) {
            if (!where(x, y))
                continue;
            const std::size_t i = static_cast<std::size_t>(y * k_w + x);
            for (unsigned sh = 0; sh < 24; sh += 8) {
                const int d = std::abs(channel(a[i], sh) - channel(b[i], sh));
                if (d > worst) {
                    worst = d;
                    if (wx)
                        *wx = x;
                    if (wy)
                        *wy = y;
                }
            }
        }
    return worst;
}

int max_diff(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    return max_diff(a, b, [](int, int) { return true; });
}

/// The close buttons of `ws` whose circle shows (hovered, pressed): a blit of the decoration
/// atlas, whose anti-aliased edge the CPU compositor blends in linear light and GL in the
/// encoded space, as every blit always was — not what these tests compare.
bool in_circle(std::span<const titled_window> ws, double scale, int x, int y) {
    for (const titled_window& w : ws) {
        if (w.button == deco_button::normal)
            continue;
        const lx::rect2i c = lx::compositor::close_rect(w.geometry);
        const int x0 = static_cast<int>(std::floor(c.x * scale)) - 1;
        const int y0 = static_cast<int>(std::floor(c.y * scale)) - 1;
        const int x1 = static_cast<int>(std::ceil((c.x + c.width) * scale)) + 1;
        const int y1 = static_cast<int>(std::ceil((c.y + c.height) * scale)) + 1;
        if (x >= x0 && x < x1 && y >= y0 && y < y1)
            return true;
    }
    return false;
}

/// The glyph boxes of `ws`' title bars (text room and close button) at scale 1: where the
/// blending space shows.
bool in_glyph_area(std::span<const titled_window> ws, int x, int y) {
    for (const titled_window& w : ws) {
        const lx::rect2i t = lx::compositor::title_rect(w.geometry);
        if (x >= t.x && x < t.x + t.width && y >= t.y + 4 && y < t.y + t.height - 4)
            return true;
    }
    return false;
}

// ── Reference images ──

std::string reference_path(const char* name) {
    std::string path = LUMEN_DECO_GOLDEN_DIR;
    path += '/';
    path += name;
    path += ".png";
    return path;
}

bool update_requested() {
    const char* v = std::getenv("LUMEN_UI_GOLDEN_UPDATE");
    return v && std::strcmp(v, "1") == 0;
}

std::vector<std::uint8_t> to_rgb(const std::vector<std::uint32_t>& px) {
    std::vector<std::uint8_t> out(px.size() * 3);
    for (std::size_t i = 0; i < px.size(); ++i) {
        out[i * 3 + 0] = static_cast<std::uint8_t>(px[i] >> 16u);
        out[i * 3 + 1] = static_cast<std::uint8_t>(px[i] >> 8u);
        out[i * 3 + 2] = static_cast<std::uint8_t>(px[i]);
    }
    return out;
}

bool write_png(const std::string& path, const std::vector<std::uint32_t>& px) {
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width = k_w;
    img.height = k_h;
    img.format = PNG_FORMAT_RGB;
    const std::vector<std::uint8_t> rgb = to_rgb(px);
    return png_image_write_to_file(&img, path.c_str(), 0, rgb.data(), 0, nullptr) != 0;
}

bool read_png(const std::string& path, std::vector<std::uint8_t>& rgb) {
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path.c_str()))
        return false;
    img.format = PNG_FORMAT_RGB;
    if (img.width != k_w || img.height != k_h) {
        png_image_free(&img);
        return false;
    }
    rgb.assign(PNG_IMAGE_SIZE(img), 0);
    return png_image_finish_read(&img, nullptr, rgb.data(), 0, nullptr) != 0;
}

/// LUMEN_DECO_DUMP=<dir>: every backend's frame written as <dir>/<name>.png.
void dump(const char* name, const std::vector<std::uint32_t>& px) {
    const char* dir = std::getenv("LUMEN_DECO_DUMP");
    if (!dir || !*dir)
        return;
    (void)write_png(std::string(dir) + "/" + name + ".png", px);
}

/// `px` against the committed reference `name`, exactly (the CPU compositor is the reference).
void check_reference(const char* name, const std::vector<std::uint32_t>& px) {
    const std::string path = reference_path(name);
    if (update_requested()) {
        LUMEN_CHECK(write_png(path, px));
        std::printf("    wrote %s\n", path.c_str());
        return;
    }
    std::vector<std::uint8_t> ref;
    const bool found = read_png(path, ref);
    if (!found)
        std::printf("    no reference %s (LUMEN_UI_GOLDEN_UPDATE=1 writes it)\n", path.c_str());
    LUMEN_CHECK(found);
    if (!found)
        return;
    const std::vector<std::uint8_t> got = to_rgb(px);
    std::size_t differ = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (got[i] != ref[i] && differ++ == 0)
            first = i / 3;
    if (differ)
        std::printf("    %s: %zu channel(s) differ, first at (%zu, %zu)\n", name, differ,
                    first % k_w, first / k_w);
    LUMEN_CHECK(differ == 0);
}

/// Ink of a title: pixels in the bar's text room brighter than the bar by more than 40.
unsigned ink_in_title(const std::vector<std::uint32_t>& px, lx::rect2i geometry, std::uint32_t bar) {
    const lx::rect2i t = lx::compositor::title_rect(geometry);
    unsigned ink = 0;
    for (int y = t.y; y < t.y + t.height; ++y)
        for (int x = t.x; x < t.x + t.width - lx::compositor::k_deco_title_height; ++x)
            if (channel(px[static_cast<std::size_t>(y * k_w + x)], 8) > channel(bar, 8) + 40)
                ++ink;
    return ink;
}

// ── GL and Vulkan environments (one each per process) ──

struct gl_env {
    int fd = -1;
    lx::gfx::egl_device device{};
    lx::gfx::gl_compositor comp{};
    lx::gfx::ui_gl_renderer ui{};
    std::optional<lx::gfx::ui_gl_target> target{};
};

gl_env* shared_gl() {
    static std::unique_ptr<gl_env> g{};
    static bool tried = false;
    if (tried)
        return g.get();
    tried = true;
    const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::printf("    (gl skip: no render node)\n");
        return nullptr;
    }
    auto device = lx::gfx::egl_device::create(fd);
    if (!device || device->is_software_renderer() || !device->gles_at_least(3, 1)) {
        std::printf("    (gl skip: %s)\n", device ? "no hardware GLES 3.1" : device.get_error().message);
        ::close(fd);
        return nullptr;
    }
    auto e = std::make_unique<gl_env>();
    e->fd = fd;
    e->device = std::move(device).value();
    if (!e->comp.initialize(e->device) || !e->ui.initialize()) {
        std::printf("    (gl skip: initialisation failed)\n");
        return nullptr;
    }
    const std::vector<unsigned char> atlas = rgba_of(deco_atlas());
    if (!e->comp.upload_rgba(lx::compositor::k_deco_atlas_texture_id, lx::compositor::k_deco_atlas_size,
                             lx::compositor::k_deco_atlas_size, atlas.data()))
        return nullptr;
    auto t = lx::gfx::ui_gl_target::create(k_w, k_h);
    if (!t)
        return nullptr;
    e->target.emplace(std::move(t).value());
    std::printf("    gl: %s, GLES %d.%d\n", e->device.renderer(), e->device.gles_major(),
                e->device.gles_minor());
    g = std::move(e);
    return g.get();
}

std::vector<std::uint32_t> paint_gl(gl_env& g, desktop& d) {
    static lx::compositor::frame_painter painter;
    const auto& snap = d.publish();
    auto p = painter.paint_gl(g.comp, &g.ui, g.target->framebuffer(), k_w, k_h, snap, full_region(),
                              &env().atlas);
    LUMEN_CHECK(p && p->paint.stream && p->ui.primitives_skipped == 0 &&
                p->composite.draws_skipped == 0);
    if (p && p->composite.out_fence_fd >= 0)
        ::close(p->composite.out_fence_fd);
    d.release(snap);
    std::vector<std::uint32_t> px(static_cast<std::size_t>(k_w * k_h), 0u);
    LUMEN_CHECK(static_cast<bool>(g.target->read_back(px)));
    return px;
}

std::string g_vk_validation_log{};
bool g_vk_validation_requested = false;

struct vk_env {
    lx::gfx::device device{};
    lx::gfx::vulkan_compositor comp{};
    lx::gfx::ui_vk_renderer ui{};
    lx::gfx::render_target target{};
};

/// Torn down by `main` (the loader and the validation layer are gone by static destruction).
std::unique_ptr<vk_env> g_vk{};

vk_env* shared_vk() {
    static bool tried = false;
    if (tried)
        return g_vk.get();
    tried = true;
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected || !selected->context().valid()) {
        std::printf("    (vk skip: no Vulkan device)\n");
        return nullptr;
    }
    auto e = std::make_unique<vk_env>();
    e->device = std::move(selected).value();
    if (auto ok = e->comp.initialize(e->device.context()); !ok) {
        std::printf("    (vk skip: %s)\n", ok.get_error().message);
        return nullptr;
    }
    if (auto ok = e->ui.initialize(e->device.context()); !ok) {
        std::printf("    (vk skip: %s)\n", ok.get_error().message);
        return nullptr;
    }
    const std::vector<unsigned char> atlas = rgba_of(deco_atlas());
    if (!e->comp.upload_rgba(lx::compositor::k_deco_atlas_texture_id, lx::compositor::k_deco_atlas_size,
                             lx::compositor::k_deco_atlas_size, atlas.data())) {
        std::printf("    (vk skip: atlas upload failed)\n");
        return nullptr;
    }
    auto t = lx::gfx::render_target::create(e->device.context(), k_w, k_h);
    if (!t) {
        std::printf("    (vk skip: %s)\n", t.get_error().message);
        return nullptr;
    }
    e->target = std::move(t).value();
    std::printf("    vk: %s%s\n", e->ui.caps().device_name,
                e->ui.caps().software ? " (software: correctness only)" : "");
    g_vk = std::move(e);
    return g_vk.get();
}

std::vector<std::uint32_t> read_vk(vk_env& v) {
    std::vector<unsigned char> rgba(static_cast<std::size_t>(k_w * k_h) * 4u);
    LUMEN_CHECK(static_cast<bool>(v.comp.read_back(v.target, rgba.data(),
                                                   static_cast<unsigned>(rgba.size()))));
    std::vector<std::uint32_t> px(static_cast<std::size_t>(k_w * k_h));
    for (std::size_t i = 0; i < px.size(); ++i)
        px[i] = 0xFF000000u | (static_cast<std::uint32_t>(rgba[i * 4]) << 16u) |
                (static_cast<std::uint32_t>(rgba[i * 4 + 1]) << 8u) | rgba[i * 4 + 2];
    return px;
}

std::vector<std::uint32_t> paint_vk(vk_env& v, desktop& d, lx::compositor::vk_paint_result* out = nullptr) {
    static lx::compositor::frame_painter painter;
    const auto& snap = d.publish();
    auto p = painter.paint_vk(v.comp, &v.ui, v.target, snap, &env().atlas);
    LUMEN_CHECK(static_cast<bool>(p));
    if (p && p->composite.out_fence_fd >= 0)
        ::close(p->composite.out_fence_fd);
    if (p && out)
        *out = p.value();
    d.release(snap);
    return read_vk(v);
}

/// Mean perceived coverage of the title text — (L − L_bar) / (L_text − L_bar) over the pixels
/// a white-on-black rendering inks — of window `w` in `px`: the stem weight `text.contrast`
/// is meant to keep.
float stem_weight(const std::vector<std::uint32_t>& px, const titled_window& w, std::uint32_t bar,
                  std::uint32_t fg) {
    const lx::rect2i t = lx::compositor::title_rect(w.geometry);
    const float lbar = static_cast<float>(channel(bar, 16) + channel(bar, 8) + channel(bar, 0)) / 3.f;
    const float lfg = static_cast<float>(channel(fg, 16) + channel(fg, 8) + channel(fg, 0)) / 3.f;
    double sum = 0.0;
    unsigned n = 0;
    for (int y = t.y; y < t.y + t.height; ++y)
        for (int x = t.x; x < t.x + t.width - lx::compositor::k_deco_title_height; ++x) {
            const std::uint32_t p = px[static_cast<std::size_t>(y * k_w + x)];
            const float l = static_cast<float>(channel(p, 16) + channel(p, 8) + channel(p, 0)) / 3.f;
            if (l <= lbar + 0.5f)
                continue;
            sum += (l - lbar) / (lfg - lbar);
            ++n;
        }
    return n ? static_cast<float>(sum / n) : 0.f;
}

} // namespace

// ── Layout ───────────────────────────────────────────────────────────────────────────

LUMEN_TEST(title_layout_keeps_the_rules_of_the_cpu_titles) {
    text_env& e = env();
    lx::compositor::title_run fits{};
    auto a = e.text.layout_title(fits, k_windows[0].title, false,
                                 lx::compositor::title_text_room(k_windows[0].geometry.width), 1.0, true);
    LUMEN_CHECK(a && a.value() && !fits.ellipsized && fits.width > 0 && !fits.glyphs.empty());
    LUMEN_CHECK(fits.width <= lx::compositor::title_text_room(k_windows[0].geometry.width));
    // The same title again: nothing to do.
    auto again = e.text.layout_title(fits, k_windows[0].title, false,
                                     lx::compositor::title_text_room(k_windows[0].geometry.width), 1.0,
                                     false);
    LUMEN_CHECK(again && !again.value());
    // Too long: cut with an ellipsis within the room; a resize that keeps the cut lays nothing
    // out, one that moves it does.
    lx::compositor::title_run cut{};
    const int room = lx::compositor::title_text_room(k_windows[1].geometry.width);
    LUMEN_CHECK(laid(e.text.layout_title(cut, k_windows[1].title, false, room, 1.0, true)));
    LUMEN_CHECK(cut.ellipsized && cut.width <= room);
    LUMEN_CHECK(kept(e.text.layout_title(cut, k_windows[1].title, false, room, 1.0, false)));
    LUMEN_CHECK(laid(e.text.layout_title(cut, k_windows[1].title, false, room + 40, 1.0, false)));
    LUMEN_CHECK(cut.width <= room + 40);
    // Not responding: the suffix always shown; with no room for the title too, the suffix
    // alone, cut itself.
    lx::compositor::title_run nr{};
    LUMEN_CHECK(laid(e.text.layout_title(nr, "Editor", true, 400, 1.0, true)));
    lx::compositor::title_run plain{};
    LUMEN_CHECK(laid(e.text.layout_title(plain, "Editor", false, 400, 1.0, true)));
    LUMEN_CHECK(nr.width > plain.width + 60 && !nr.ellipsized);
    lx::compositor::title_run tight{};
    const int small = lx::compositor::title_text_room(k_windows[2].geometry.width);
    LUMEN_CHECK(laid(e.text.layout_title(tight, "Editor", true, small, 1.0, true)));
    LUMEN_CHECK(tight.width > 0 && tight.width <= small);
    // At scale 2 the run is laid out in device pixels: about twice as wide.
    lx::compositor::title_run twice{};
    LUMEN_CHECK(laid(e.text.layout_title(twice, "Editor", false, 400, 2.0, true)));
    LUMEN_CHECK(twice.width >= 2 * plain.width - 4 && twice.width <= 2 * plain.width + 4);
    // What a title change costs on the UI thread (not a frame cost): a title in a font size
    // not seen yet rasterises its glyphs; the same text again (another window, a refit) finds
    // them in the glyph cache.
    {
        const auto us = [](auto t0) {
            return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        };
        lx::compositor::title_run r{};
        auto t0 = std::chrono::steady_clock::now();
        LUMEN_CHECK(laid(e.text.layout_title(r, k_windows[0].title, false, 400, 1.75, true)));
        const double cold = us(t0);
        t0 = std::chrono::steady_clock::now();
        LUMEN_CHECK(laid(e.text.layout_title(r, k_windows[0].title, false, 400, 1.75, true)));
        const double warm = us(t0);
        std::printf("    title layout (%zu glyphs): %.0f us rasterising, %.0f us from the glyph cache\n",
                    r.glyphs.size(), cold, warm);
    }
    // An empty title draws nothing.
    lx::compositor::title_run none{};
    (void)e.text.layout_title(none, "", false, 400, 1.0, true);
    LUMEN_CHECK(none.width == 0 && none.glyphs.empty());
    // The run a view draws: its own scale, else the largest.
    lx::compositor::title_text tt{};
    tt.runs[0].scale = 1.0;
    tt.runs[1].scale = 2.0;
    LUMEN_CHECK(tt.for_scale(1.0) == &tt.runs[0] && tt.for_scale(2.0) == &tt.runs[1]);
    LUMEN_CHECK(tt.for_scale(1.5) == &tt.runs[1]);
}

// The manager keeps one run per output scale the frame is on, lays a title out again only
// when its text, its cut or its scales change, and rasterises the close glyph per scale.
LUMEN_TEST(manager_lays_titles_out_per_scale) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::compositor::chrome_atlas atlas{};
    lx::compositor::decoration_manager m{};
    m.bind_chrome(&atlas);
    LUMEN_CHECK(m.text() != nullptr);
    m.text()->set_font({lx::text::k_ui_family, 10.f, lx::text::weight::bold, lx::text::style::normal});
    lx::compositor::toplevel_record rec{};
    rec.id = lx::toplevel_id{7};
    rec.surface = lx::surface_id{70};
    rec.title = "Terminal";
    rec.mapped = true;
    rec.decoration_current = lx::compositor::decoration_mode::server;
    rec.geometry = {40, 60, 300, 200};
    m.sync(rec);
    m.on_tick();
    const lx::compositor::frame_node* n = m.node(rec.id);
    LUMEN_CHECK(n && n->title && n->text == m.text());
    const auto runs = [&] {
        unsigned k = 0;
        for (const lx::compositor::title_run& r : n->title->runs)
            k += r.scale > 0.0 && r.width > 0 ? 1u : 0u;
        return k;
    };
    LUMEN_CHECK(runs() == 1 && n->title->for_scale(1.0) && n->title->for_scale(1.0)->scale == 1.0);
    LUMEN_CHECK(m.text()->find_close(1.0) != nullptr);
    const std::size_t glyphs = n->title->for_scale(1.0)->glyphs.size();
    LUMEN_CHECK(glyphs >= 7); // "Terminal": a glyph per letter (bar ligatures)
    // On two outputs, 1 and 1.5: a run each, both rasterised 1:1, the close glyph at both.
    const double two[2] = {1.0, 1.5};
    m.set_window_scales(rec.id, two);
    m.on_tick();
    LUMEN_CHECK(runs() == 2 && n->title->for_scale(1.5)->scale == 1.5);
    LUMEN_CHECK(m.text()->find_close(1.5) != nullptr && m.text()->find_close(1.5)->size == 15);
    // A move (same width) and a focus change lay nothing out: the glyph storage is untouched.
    const lx::compositor::title_glyph* before = n->title->for_scale(1.0)->glyphs.data();
    rec.geometry.x += 25;
    m.sync(rec);
    m.set_active(rec.id);
    m.on_tick();
    LUMEN_CHECK(n->title->for_scale(1.0)->glyphs.data() == before &&
                n->title->for_scale(1.0)->glyphs.size() == glyphs);
    // Back on one output: the 1.5 run goes.
    const double one[1] = {1.0};
    m.set_window_scales(rec.id, one);
    m.on_tick();
    LUMEN_CHECK(runs() == 1);
    // Not responding: the suffix joins the title, and the frame dims.
    rec.not_responding = true;
    m.sync(rec);
    m.on_tick();
    LUMEN_CHECK(n->dimmed && n->title->for_scale(1.0)->glyphs.size() > glyphs + 10);
    // A new title.
    rec.not_responding = false;
    rec.title = "Files";
    m.sync(rec);
    m.on_tick();
    LUMEN_CHECK(!n->dimmed && n->title->for_scale(1.0)->glyphs.size() == 5);
    m.on_destroyed(rec.id, rec.surface);
    LUMEN_CHECK(m.node(rec.id) == nullptr);
}

// ── CPU: the reference ───────────────────────────────────────────────────────────────

LUMEN_TEST(cpu_titles_match_the_reference) {
    desktop d{k_windows};
    const std::vector<std::uint32_t> px = paint_cpu(d);
    check_reference("titles", px);
    // What the reference holds: the bars' colours (the dimmed one with the CPU's byte rule),
    // ink in every title, the close glyph's light centre on the active frame.
    const auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y * k_w + x)] & 0xFFFFFFu; };
    const lx::rect2i ta = lx::compositor::title_rect(k_windows[0].geometry);
    const lx::rect2i tc = lx::compositor::title_rect(k_windows[2].geometry);
    LUMEN_CHECK(at(ta.x + 2, ta.y + 2) == 0x303033u);
    LUMEN_CHECK(at(tc.x + 2, tc.y + 2) == dimmed(0x48484Cu));
    LUMEN_CHECK(ink_in_title(px, k_windows[0].geometry, 0x303033u) > 150);
    LUMEN_CHECK(ink_in_title(px, k_windows[1].geometry, 0x48484Cu) > 60);
    LUMEN_CHECK(ink_in_title(px, k_windows[2].geometry, dimmed(0x48484Cu)) > 30);
    const lx::rect2i ca = lx::compositor::close_rect(k_windows[0].geometry);
    LUMEN_CHECK(channel(at(ca.x + ca.width / 2, ca.y + ca.height / 2), 16) > 0xC0);
    // The hovered button's circle under the glyph.
    const lx::rect2i cb = lx::compositor::close_rect(k_windows[1].geometry);
    LUMEN_CHECK(at(cb.x + 6, cb.y + cb.height / 2) == 0xE0443Eu);
}

// At 1.25 the titles are laid out at 1.25 and drawn 1:1 (the sprites sit on whole pixels at
// their tiles' size), not resampled from a scale-1 raster.
LUMEN_TEST(cpu_titles_at_a_fractional_scale_are_drawn_one_to_one) {
    desktop d{k_windows_125, 1.25};
    const auto& snap = d.publish();
    unsigned sprites = 0;
    unsigned one_to_one = 0;
    for (const lx::gfx::sprite_instance& sp : snap.draws().sprite_instances()) {
        ++sprites;
        const bool whole = sp.bounds[0] == std::floor(sp.bounds[0]) && sp.bounds[1] == std::floor(sp.bounds[1]);
        if (whole && sp.bounds[2] == static_cast<float>(sp.uv[2] - sp.uv[0]) &&
            sp.bounds[3] == static_cast<float>(sp.uv[3] - sp.uv[1]))
            ++one_to_one;
    }
    d.release(snap);
    LUMEN_CHECK(sprites > 20 && one_to_one == sprites);
    check_reference("titles-125", paint_cpu(d));
    // A run of another scale is resampled: drawn on the 1.25 output from a scale-1 raster.
    desktop resampled{k_windows_125, 1.25, lx::buffer_transform::normal, true, 1.0};
    const auto& rs = resampled.publish();
    unsigned scaled = 0;
    for (const lx::gfx::sprite_instance& sp : rs.draws().sprite_instances())
        scaled += sp.bounds[2] != static_cast<float>(sp.uv[2] - sp.uv[0]) ? 1u : 0u;
    resampled.release(rs);
    LUMEN_CHECK(scaled > 0);
}

// A turned output draws the titles turned: rendering the content space and turning the
// pixels gives the output's frame exactly (the sprites sample through the transform's basis).
LUMEN_TEST(cpu_titles_on_a_turned_output) {
    desktop content{k_windows};
    const std::vector<std::uint32_t> base = paint_cpu(content);
    for (const lx::buffer_transform t :
         {lx::buffer_transform::rotate_180, lx::buffer_transform::flipped,
          lx::buffer_transform::flipped_180}) {
        desktop turned{k_windows, 1.0, t};
        const std::vector<std::uint32_t> out = paint_cpu(turned);
        unsigned differ = 0;
        for (int y = 0; y < k_h; ++y)
            for (int x = 0; x < k_w; ++x) {
                const lx::rect2i o = lx::gfx::transform_rect({x, y, 1, 1}, t, k_w, k_h);
                differ += (base[static_cast<std::size_t>(y * k_w + x)] & 0xFFFFFFu) !=
                                  (out[static_cast<std::size_t>(o.y * k_w + o.x)] & 0xFFFFFFu)
                              ? 1u
                              : 0u;
            }
        if (differ)
            std::printf("    transform %u: %u pixel(s) differ\n", static_cast<unsigned>(t), differ);
        LUMEN_CHECK(differ == 0);
    }
}

// The same on a 90° output: the view is k_h × k_w content turned into the k_w × k_h pixels.
LUMEN_TEST(cpu_titles_on_a_quarter_turned_output) {
    // Windows that fit a 176 × 320 content space.
    const titled_window narrow[2] = {
        {{8, 40, 160, 36}, "Terminal \xE2\x80\x94 ~/src", true, false, deco_button::normal,
         lx::color::rgb(0.10f, 0.12f, 0.15f)},
        {{8, 120, 160, 60}, "A long title cut short here", false, true, deco_button::hover,
         lx::color::rgb(0.85f, 0.85f, 0.82f)},
    };
    for (const lx::buffer_transform t : {lx::buffer_transform::rotate_90, lx::buffer_transform::rotate_270,
                                         lx::buffer_transform::flipped_90}) {
        // Content space rendered by hand: k_h wide, k_w tall.
        desktop turned{narrow, 1.0, t};
        const std::vector<std::uint32_t> out = paint_cpu(turned);
        // Upright: the same windows on a k_h × k_w view of their own.
        lx::gfx::cpu_compositor& comp = cpu_comp();
        static lx::gfx::ui_cpu_renderer ui;
        lx::compositor::frame_painter painter;
        desktop upright{narrow, 1.0, lx::buffer_transform::normal};
        lx::scene::output_view_config v{};
        v.logical = {0, 0, k_h, k_w};
        v.projection = {{0, 0}, 1.0, {k_h, k_w}, lx::buffer_transform::normal};
        upright.scene->set_output_view(0, v);
        std::vector<std::uint32_t> base(static_cast<std::size_t>(k_w * k_h), 0u);
        const auto& snap = upright.publish();
        LUMEN_CHECK(static_cast<bool>(painter.paint_cpu(comp, &ui, surface_of(base, k_h, k_w), snap,
                                                        full_region(), &env().atlas)));
        upright.release(snap);
        unsigned differ = 0;
        for (int y = 0; y < k_w; ++y)
            for (int x = 0; x < k_h; ++x) {
                const lx::rect2i o = lx::gfx::transform_rect({x, y, 1, 1}, t, k_h, k_w);
                const std::uint32_t a = base[static_cast<std::size_t>(y * k_h + x)] & 0xFFFFFFu;
                const std::uint32_t b = out[static_cast<std::size_t>(o.y * k_w + o.x)] & 0xFFFFFFu;
                if (a != b && differ++ < 8)
                    std::printf("    (%d, %d): upright %06x turned %06x\n", x, y, a, b);
            }
        if (differ)
            std::printf("    transform %u: %u pixel(s) differ\n", static_cast<unsigned>(t), differ);
        LUMEN_CHECK(differ == 0);
    }
}

// Screen capture's re-composite draws the stream (titles included) into the capture buffer,
// moved by the crop: what the output showed, cropped.
LUMEN_TEST(capture_recomposite_draws_the_titles) {
    desktop d{k_windows};
    const std::vector<std::uint32_t> full = paint_cpu(d);
    lx::gfx::cpu_compositor& comp = cpu_comp();
    lx::gfx::ui_cpu_renderer ui;
    lx::compositor::frame_painter painter;
    const lx::rect2i crop{40, 20, 200, 120};
    std::vector<std::uint32_t> out(static_cast<std::size_t>(crop.width * crop.height), 0u);
    const auto& snap = d.publish();
    std::vector<lx::gfx::blit_command> blits(painter.surfaces(snap).begin(), painter.surfaces(snap).end());
    for (lx::gfx::blit_command& b : blits) {
        b.dst.x -= crop.x;
        b.dst.y -= crop.y;
        if (b.clip.width > 0 && b.clip.height > 0) {
            b.clip.x -= crop.x;
            b.clip.y -= crop.y;
        }
    }
    lx::gfx::damage_region region{};
    region.add({0, 0, crop.width, crop.height});
    LUMEN_CHECK(static_cast<bool>(painter.paint_cpu_capture(comp, ui, surface_of(out, crop.width, crop.height),
                                                            snap, blits, {crop.x, crop.y}, k_black, region,
                                                            &env().atlas)));
    d.release(snap);
    unsigned differ = 0;
    for (int y = 0; y < crop.height; ++y)
        for (int x = 0; x < crop.width; ++x)
            differ += (out[static_cast<std::size_t>(y * crop.width + x)] & 0xFFFFFFu) !=
                              (full[static_cast<std::size_t>((y + crop.y) * k_w + x + crop.x)] & 0xFFFFFFu)
                          ? 1u
                          : 0u;
    LUMEN_CHECK(differ == 0);
}

// The headless dump walks the stream: the titles' ink is there.
LUMEN_TEST(headless_dump_draws_the_titles) {
    desktop d{k_windows};
    lx::gfx::headless_backend h{};
    LUMEN_CHECK(static_cast<bool>(h.create(k_w, k_h)));
    lx::gfx::frame_renderer r{};
    lx::compositor::frame_painter painter;
    LUMEN_CHECK(static_cast<bool>(h.begin_frame()));
    LUMEN_CHECK(static_cast<bool>(h.clear(k_black)));
    const auto& snap = d.publish();
    lx::compositor::frame_paint_stats st{};
    painter.paint_headless(r, h, snap, &env().atlas, &st);
    d.release(snap);
    LUMEN_CHECK(st.dropped == 0);
    const unsigned char* rgb = h.pixel_data();
    std::vector<std::uint32_t> px(static_cast<std::size_t>(k_w * k_h));
    for (std::size_t i = 0; i < px.size(); ++i)
        px[i] = (static_cast<std::uint32_t>(rgb[i * 3]) << 16u) |
                (static_cast<std::uint32_t>(rgb[i * 3 + 1]) << 8u) | rgb[i * 3 + 2];
    LUMEN_CHECK(ink_in_title(px, k_windows[0].geometry, 0x303033u) > 150);
    LUMEN_CHECK(ink_in_title(px, k_windows[2].geometry, dimmed(0x48484Cu)) > 30);
    (void)h.end_frame_and_dump(nullptr);
}

// A steady frame — emit, visibility, order, batch, then the CPU paint — allocates nothing.
LUMEN_TEST(steady_titled_frame_allocates_nothing) {
    desktop d{k_windows};
    lx::gfx::cpu_compositor& comp = cpu_comp();
    lx::gfx::ui_cpu_renderer ui;
    lx::compositor::frame_painter painter;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(k_w * k_h), 0u);
    const lx::gfx::pixel_surface s = surface_of(px, k_w, k_h);
    unsigned long long during = 0;
    for (unsigned f = 0; f < 40; ++f) {
        const unsigned long long before = g_allocations.load();
        const auto& snap = d.publish();
        (void)painter.paint_cpu(comp, &ui, s, snap, full_region(), &env().atlas);
        d.release(snap);
        if (f >= 8)
            during += g_allocations.load() - before;
    }
    std::printf("    allocations over 32 steady frames: %llu\n", during);
    LUMEN_CHECK(during == 0);
}

// ── GL ───────────────────────────────────────────────────────────────────────────────

// Both blend UI primitives in the encoded space: G10's 3/255 everywhere, titles included.
LUMEN_TEST(gl_titles_match_cpu) {
    gl_env* g = shared_gl();
    if (!g)
        return;
    for (const double scale : {1.0, 1.25}) {
        desktop d = scale == 1.0 ? desktop{k_windows} : desktop{k_windows_125, 1.25};
        const std::vector<std::uint32_t> cpu = paint_cpu(d);
        const std::vector<std::uint32_t> gl = paint_gl(*g, d);
        dump(scale == 1.0 ? "cpu" : "cpu-125", cpu);
        dump(scale == 1.0 ? "gl" : "gl-125", gl);
        const std::span<const titled_window> ws = scale == 1.0 ? std::span<const titled_window>{k_windows}
                                                               : std::span<const titled_window>{k_windows_125};
        int x = 0, y = 0;
        const int worst = max_diff(cpu, gl, [&](int px, int py) { return !in_circle(ws, scale, px, py); },
                                   &x, &y);
        std::printf("    scale %.2f: max channel error %d/255 vs CPU at (%d, %d) (button circles aside)\n",
                    scale, worst, x, y);
        LUMEN_CHECK(worst <= 3);
        if (scale == 1.0) {
            // The dimmed frame: its fill and its glyphs' full-coverage pixels carry the CPU's
            // dimmed bytes on GL too (Phase 1's dimmed title blit was tinted another way).
            const lx::rect2i tc = lx::compositor::title_rect(k_windows[2].geometry);
            LUMEN_CHECK((gl[static_cast<std::size_t>((tc.y + 2) * k_w + tc.x + 2)] & 0xFFFFFFu) ==
                        dimmed(0x48484Cu));
        }
    }
}

// ── Vulkan: the linear F16 pass ──────────────────────────────────────────────────────

// Fills, borders and content (outside the title bars' text) within 2/255 — the encode pass's
// rounding of colours the pass decoded (measured: 1/255). The glyph edges are blended in linear
// light with `k_text_contrast_dark` (light text on dark chrome) where the CPU blends coverage
// in the encoded space, so they cannot match bit for bit: measured on lavapipe the largest
// edge difference is 9/255 at scale 1 and 6/255 at 1.25, and the mean perceived stem weight is
// +1.5 % of the CPU's — inside the 3 % the exponent was fitted to (IBM Plex Sans regular,
// 10–16 px, §4.5; these are bold 13 px titles). Without the exponent (1.0) the same text is
// visibly heavier, which the test shows too. 16/255 (the measurement with room for a hardware
// driver's own pow and rounding) and 3 % are the bounds.
LUMEN_TEST(vk_titles_in_the_linear_pass) {
    vk_env* v = shared_vk();
    if (!v)
        return;
    for (const double scale : {1.0, 1.25}) {
        const std::span<const titled_window> ws =
            scale == 1.0 ? std::span<const titled_window>{k_windows} : std::span<const titled_window>{k_windows_125};
        desktop d{ws, scale};
        const std::vector<std::uint32_t> cpu = paint_cpu(d);
        lx::compositor::vk_paint_result res{};
        const std::vector<std::uint32_t> vk = paint_vk(*v, d, &res);
        dump(scale == 1.0 ? "vk" : "vk-125", vk);
        LUMEN_CHECK(res.paint.stream && res.ui.primitives_skipped == 0 && res.composite.draws_skipped == 0);
        LUMEN_CHECK(res.ui.atlas_copies <= 1); // one page, one copy
        // Glyph areas at this scale: the title bars, projected.
        const auto glyphs = [&](int x, int y) {
            return in_glyph_area(ws, static_cast<int>(x / scale), static_cast<int>(y / scale));
        };
        int x = 0, y = 0;
        const int flat = max_diff(cpu, vk, [&](int px, int py) {
            return !glyphs(px, py) && !in_circle(ws, scale, px, py);
        }, &x, &y);
        int gx = 0, gy = 0;
        const int edges = max_diff(cpu, vk, [&](int px, int py) {
            return glyphs(px, py) && !in_circle(ws, scale, px, py);
        }, &gx, &gy);
        std::printf("    scale %.2f: outside the titles %d/255 (at %d, %d); glyph edges %d/255 (at %d, %d)\n",
                    scale, flat, x, y, edges, gx, gy);
        LUMEN_CHECK(flat <= 2);
        LUMEN_CHECK(edges <= 16);
        if (scale == 1.0) {
            const float wc = stem_weight(cpu, k_windows[0], 0x303033u, 0xF2F2F2u);
            const float wv = stem_weight(vk, k_windows[0], 0x303033u, 0xF2F2F2u);
            // The same frame with no contrast correction (exponent 1).
            static lx::compositor::frame_painter plain;
            const auto& snap = d.publish();
            auto p = plain.paint_vk(v->comp, &v->ui, v->target, snap, &env().atlas, 1.f);
            LUMEN_CHECK(static_cast<bool>(p));
            if (p && p->composite.out_fence_fd >= 0)
                ::close(p->composite.out_fence_fd);
            d.release(snap);
            const float w1 = stem_weight(read_vk(*v), k_windows[0], 0x303033u, 0xF2F2F2u);
            std::printf("    stem weight: CPU %.4f, Vulkan %.4f (%+.1f %%); exponent 1.0: %.4f (%+.1f %%)\n",
                        wc, wv, 100.f * (wv - wc) / wc, w1, 100.f * (w1 - wc) / wc);
            LUMEN_CHECK(wc > 0.f && std::fabs(wv - wc) / wc <= 0.03f);
            LUMEN_CHECK(std::fabs(wv - wc) < std::fabs(w1 - wc));
            // Dimmed: the fill as on the CPU, and the glyph pixels the CPU drew at full
            // coverage carry the same dimmed byte (one colour rule on every backend).
            const lx::rect2i tc = lx::compositor::title_rect(k_windows[2].geometry);
            const std::size_t fill = static_cast<std::size_t>((tc.y + 2) * k_w + tc.x + 2);
            LUMEN_CHECK(std::abs(channel(vk[fill], 8) - channel(dimmed(0x48484Cu), 8)) <= 1);
            // The inactive glyph colour, dimmed.
            const int dim = channel(dimmed(0xA8A8ACu), 8);
            unsigned full_cov = 0;
            int worst_core = 0;
            for (int py = tc.y; py < tc.y + tc.height; ++py)
                for (int px = tc.x; px < tc.x + tc.width - lx::compositor::k_deco_title_height; ++px) {
                    const std::size_t i = static_cast<std::size_t>(py * k_w + px);
                    if (channel(cpu[i], 8) != dim)
                        continue;
                    ++full_cov;
                    worst_core = std::max(worst_core, std::abs(channel(vk[i], 8) - dim));
                }
            std::printf("    dimmed glyph cores: %u px, max error %d/255\n", full_cov, worst_core);
            LUMEN_CHECK(full_cov > 10 && worst_core <= 2);
        }
    }
}

// A frame without primitives is the blit composite it always was, byte for byte, and no UI
// renderer runs.
LUMEN_TEST(vk_frame_without_primitives_is_the_blit_composite) {
    vk_env* v = shared_vk();
    if (!v)
        return;
    desktop d{k_windows, 1.0, lx::buffer_transform::normal, false};
    lx::compositor::vk_paint_result res{};
    const std::vector<std::uint32_t> painted = paint_vk(*v, d, &res);
    LUMEN_CHECK(!res.paint.stream && res.ui.draw_calls == 0);
    lx::compositor::frame_painter painter;
    const auto& snap = d.publish();
    const auto blits = painter.surfaces(snap);
    auto c = v->comp.composite(v->target, snap.clear_color(), blits.data(), static_cast<unsigned>(blits.size()));
    LUMEN_CHECK(static_cast<bool>(c));
    if (c && c->out_fence_fd >= 0)
        ::close(c->out_fence_fd);
    d.release(snap);
    LUMEN_CHECK(read_vk(*v) == painted);
}

// A UI renderer that recorded the compositor's texture (a `surface` ref drawn by its own
// `draw`) is waited for before the compositor destroys it; a texture no external frame
// recorded is destroyed without asking.
LUMEN_TEST(vk_compositor_waits_for_external_users) {
    vk_env* v = shared_vk();
    if (!v)
        return;
    static unsigned waits = 0;
    const auto wait = [](void*) { ++waits; };
    int marker = 0;
    LUMEN_CHECK(v->comp.add_external_user({&marker, wait, nullptr}));
    const unsigned char texel[4] = {255, 0, 0, 255};
    LUMEN_CHECK(static_cast<bool>(v->comp.upload_rgba(0x5151u, 1, 1, texel)));
    LUMEN_CHECK(static_cast<bool>(v->comp.upload_rgba(0x5152u, 1, 1, texel)));
    lx::gfx::blit_command b{};
    b.texture_id = 0x5151u;
    b.dst = {0, 0, 8, 8};
    b.opacity = 1.f;
    const lx::gfx::primitive_ref ref{1, lx::gfx::primitive_kind::surface, 0, 0, 0};
    const lx::gfx::batch run{lx::gfx::primitive_kind::surface, 0, 0, 1, {0, 0, 8, 8}};
    lx::gfx::ui_stream stream{};
    stream.refs = {&ref, 1};
    stream.batches = {&run, 1};
    stream.surfaces = {&b, 1};
    auto target = lx::gfx::ui_vk_target::create(v->device.context(), 16, 16);
    LUMEN_CHECK(static_cast<bool>(target));
    lx::gfx::ui_vk_renderer ui{};
    LUMEN_CHECK(static_cast<bool>(ui.initialize(v->device.context())));
    ui.set_surface_compositor(&v->comp);
    auto drawn = ui.draw(*target, stream, nullptr, full_region(), k_black);
    LUMEN_CHECK(drawn && drawn->draw_calls == 1);
    // Not recorded by anyone else: no wait.
    v->comp.forget_texture(0x5152u);
    LUMEN_CHECK(waits == 0);
    // Recorded by `ui`'s frame: every external user is waited for first.
    v->comp.forget_texture(0x5151u);
    LUMEN_CHECK(waits == 1);
    ui.set_surface_compositor(nullptr);
    v->comp.remove_external_user(&marker);
    LUMEN_CHECK(waits == 2); // removal waits for what the user may have recorded
    ui.wait_idle();
}

// The titled desktop drawn as the compositor draws an output on Vulkan: three scanout slots
// in rotation, each frame's region its damage plus the slot's age, only the damage
// composited into the persistent intermediate (titles, fills and surfaces through the UI
// renderer's linear pass, one rect at a time). Every frame equals a full repaint of the
// same snapshot by a compositor of its own, byte for byte.
LUMEN_TEST(vk_damage_limited_titles_match_a_full_repaint) {
    vk_env* v = shared_vk();
    if (!v)
        return;
    lx::gfx::vulkan_compositor ref{};
    lx::gfx::ui_vk_renderer ref_ui{};
    if (!ref.initialize(v->device.context()) || !ref_ui.initialize(v->device.context())) {
        std::printf("    SKIP: no second compositor\n");
        return;
    }
    const std::vector<unsigned char> atlas = rgba_of(deco_atlas());
    LUMEN_CHECK(static_cast<bool>(ref.upload_rgba(lx::compositor::k_deco_atlas_texture_id,
                                                  lx::compositor::k_deco_atlas_size,
                                                  lx::compositor::k_deco_atlas_size, atlas.data())));
    lx::gfx::render_target slots[3]{};
    lx::gfx::render_target ref_target{};
    for (auto* t : {&slots[0], &slots[1], &slots[2], &ref_target}) {
        auto made = lx::gfx::render_target::create(v->device.context(), k_w, k_h);
        if (!made) {
            std::printf("    SKIP: %s\n", made.get_error().message);
            return;
        }
        *t = std::move(made).value();
    }
    const auto read = [](lx::gfx::vulkan_compositor& c, const lx::gfx::render_target& t) {
        std::vector<unsigned char> px(static_cast<std::size_t>(k_w * k_h) * 4u);
        LUMEN_CHECK(static_cast<bool>(c.read_back(t, px.data(), static_cast<unsigned>(px.size()))));
        return px;
    };
    const auto frame_rect = [](const lx::rect2i g) {
        return lx::rect2i{g.x - 8, g.y - 40, g.width + 16, g.height + 48}; // title bar, borders
    };

    desktop d{k_windows};
    lx::compositor::frame_painter painter;
    lx::compositor::frame_painter ref_painter;
    lx::gfx::damage_region age[3]{};
    for (auto& a : age)
        a.set_full();
    const lx::gfx::render_target* previous = nullptr;
    unsigned partial = 0;
    for (unsigned i = 0; i < 10; ++i) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        if (i == 0) {
            d.scene->note_full_damage_output(0);
        } else if (i % 3 == 1) {
            // Window 0 (the terminal) moves right: its frame and surface, old and new place.
            const lx::rect2i old = d.frames[0]->geometry;
            d.frames[0]->geometry.x += 6;
            d.windows[0]->set_transform({d.frames[0]->geometry.x, d.frames[0]->geometry.y});
            d.scene->note_damage(frame_rect(old));
            d.scene->note_damage(frame_rect(d.frames[0]->geometry));
        } else if (i % 3 == 2) {
            // Window 2 recoloured.
            d.windows[2]->set_solid(true, i % 2 ? lx::color::rgb(0.6f, 0.2f, 0.1f)
                                                : lx::color::rgb(0.1f, 0.5f, 0.3f));
            d.scene->note_damage(k_windows[2].geometry);
        } else {
            // Window 1's close button hovered / not.
            d.frames[1]->button = i % 2 ? deco_button::normal : deco_button::hover;
            d.scene->note_damage(frame_rect(k_windows[1].geometry));
        }
        LUMEN_CHECK(d.scene->commit_frame(0, ++d.frame_index));
        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        const auto& snap = d.scene->acquire_render_snapshot(0);

        lx::gfx::damage_region changed = snap.damage_rects();
        changed.clip({0, 0, k_w, k_h});
        const unsigned back = i % 3u;
        lx::gfx::damage_region region = changed;
        region.add_region(age[back]);
        region.clip({0, 0, k_w, k_h});
        auto p = painter.paint_vk(v->comp, &v->ui, slots[back], snap, &env().atlas,
                                  lx::gfx::k_text_contrast_dark, &region, &changed, previous);
        LUMEN_CHECK(p && p->paint.stream && p->ui.primitives_skipped == 0 &&
                    p->composite.draws_skipped == 0);
        if (p && p->composite.out_fence_fd >= 0)
            ::close(p->composite.out_fence_fd);
        if (p && !changed.full)
            partial += p->ui.batches_skipped > 0 ? 1u : 0u; // a batch met no damage rect
        auto r = ref_painter.paint_vk(ref, &ref_ui, ref_target, snap, &env().atlas);
        LUMEN_CHECK(static_cast<bool>(r));
        if (r && r->composite.out_fence_fd >= 0)
            ::close(r->composite.out_fence_fd);
        d.release(snap);
        for (unsigned s = 0; s < 3; ++s)
            if (s != back)
                age[s].add_region(changed);
        age[back].clear();
        previous = &slots[back];
        LUMEN_CHECK(read(v->comp, slots[back]) == read(ref, ref_target));
    }
    std::printf("    %u of 9 damage frames limited to their rects\n", partial);
    LUMEN_CHECK(partial > 0);
    ref_ui.wait_idle();
    ref_ui.shutdown();
    ref.shutdown();
}

// GPU timing on a frame with primitives (`begin_linear_frame` … `end_linear_frame`, the UI
// stream recorded in the linear pass): the frame is timed, its composite and encode passes
// come back non-zero and inside the frame's wall time, and its pixels are the untimed frame's.
// Under the validation run this also checks the query pool's reset/write/read sequence.
LUMEN_TEST(vk_gpu_timing_times_the_linear_pass) {
    vk_env* v = shared_vk();
    if (!v)
        return;
    if (!v->comp.gpu_timing_supported()) {
        std::printf("    SKIP: queue family has no timestamps\n");
        return;
    }
    desktop d{k_windows, 1.0};
    lx::compositor::vk_paint_result res{};
    const std::vector<std::uint32_t> untimed = paint_vk(*v, d, &res);
    LUMEN_CHECK(res.paint.stream && res.composite.gpu_serial == 0);
    lx::gfx::composite_gpu_time t[lx::gfx::vulkan_compositor::k_gpu_times]{};
    LUMEN_CHECK(v->comp.take_gpu_times(t, lx::gfx::vulkan_compositor::k_gpu_times) == 0);
    v->comp.set_gpu_timing(true);
    for (int i = 0; i < 3; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        // Read back inside: the frame has finished when this returns.
        const std::vector<std::uint32_t> timed = paint_vk(*v, d, &res);
        const auto wall_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
                .count());
        LUMEN_CHECK(res.paint.stream && res.composite.gpu_serial != 0);
        LUMEN_CHECK(timed == untimed);
        const unsigned n = v->comp.take_gpu_times(t, lx::gfx::vulkan_compositor::k_gpu_times);
        LUMEN_CHECK(n == 1);
        if (n != 1)
            continue;
        std::printf("    frame %d: composite %.3f ms, encode %.3f ms, wall %.3f ms\n", i,
                    t[0].composite_ns / 1e6, t[0].encode_ns / 1e6, wall_ns / 1e6);
        LUMEN_CHECK(t[0].serial == res.composite.gpu_serial);
        LUMEN_CHECK(t[0].composite_ns > 0 && t[0].encode_ns > 0);
        LUMEN_CHECK(t[0].composite_ns + t[0].encode_ns <= wall_ns);
    }
    v->comp.set_gpu_timing(false);
    (void)paint_vk(*v, d, &res);
    LUMEN_CHECK(res.composite.gpu_serial == 0);
    LUMEN_CHECK(v->comp.take_gpu_times(t, lx::gfx::vulkan_compositor::k_gpu_times) == 0);
}

// Last: with LUMEN_VULKAN_VALIDATION=1, the validation layer's log holds no error.
LUMEN_TEST(vk_validation_clean) {
    if (!g_vk_validation_requested)
        return;
    LUMEN_CHECK(!g_vk_validation_log.empty());
    if (g_vk_validation_log.empty())
        return;
    vk_env* v = shared_vk();
    if (!v) {
        std::printf("    SKIP: no Vulkan device, nothing validated\n");
        return;
    }
    v->ui.wait_idle();
    std::FILE* f = std::fopen(g_vk_validation_log.c_str(), "r");
    LUMEN_CHECK(f != nullptr);
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
    std::printf("    validation layer log %s: %u lines, %u errors\n", g_vk_validation_log.c_str(), lines,
                errors);
    LUMEN_CHECK(errors == 0);
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    if (const char* v = std::getenv("LUMEN_VULKAN_VALIDATION"); v && *v == '1') {
        g_vk_validation_requested = true;
        const char* found = std::getenv("LUMEN_VK_VALIDATION_LAYER");
        const std::string manifest =
            found && *found ? found : "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json";
        if (::access(manifest.c_str(), R_OK) == 0) {
            g_vk_validation_log = "/tmp/lumen-deco-vk-validation-" + std::to_string(::getpid()) + ".log";
            ::setenv("VK_ADD_LAYER_PATH", manifest.substr(0, manifest.find_last_of('/')).c_str(), 1);
            ::setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
            ::setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG", 1);
            ::setenv("VK_KHRONOS_VALIDATION_LOG_FILENAME", g_vk_validation_log.c_str(), 1);
            ::setenv("VK_KHRONOS_VALIDATION_REPORT_FLAGS", "error,warn", 1);
            std::remove(g_vk_validation_log.c_str());
        } else {
            std::printf("LUMEN_VULKAN_VALIDATION: %s not readable\n", manifest.c_str());
        }
    }
    const int status = lumen_test::run_all(argc, argv);
    g_vk.reset();
    if (!g_vk_validation_log.empty())
        std::remove(g_vk_validation_log.c_str());
    return status;
}
