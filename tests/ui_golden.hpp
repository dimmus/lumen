// UI primitive golden images (ADR-010 §8.4): the case set, the reference renderer's output
// for it, and the comparison every backend's test runs against the committed references.
//
// The references in tests/golden/ui/*.png are what `gfx::ui_cpu_renderer` draws for each
// case below, as 8-bit RGB (the target is XRGB8888 over an opaque clear, so alpha carries
// nothing). The CPU renderer must reproduce them bit for bit; the GL and Vulkan stages
// render the same `cases()` through their own backend and call `compare_xrgb` with the
// G10 tolerance (`k_tolerance_vs_cpu`, 3/255 per channel).
//
// A reference changes only as a reviewed change: rerun test_ui_golden with
// LUMEN_UI_GOLDEN_UPDATE=1, look at every rewritten PNG, and commit them with the change
// that moved them. LUMEN_UI_GOLDEN_DIR overrides where the references are read and written.

#pragma once

#include <png.h>
#include <stdlib.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.scene;
import lx.text;
import lx.ui;

#ifndef LUMEN_UI_GOLDEN_DIR
#define LUMEN_UI_GOLDEN_DIR "tests/golden/ui"
#endif
#ifndef LUMEN_TEST_FONT_DIR
#define LUMEN_TEST_FONT_DIR "tests/fonts"
#endif

namespace ui_golden {

/// G10: a backend's pixel against the CPU reference, per channel.
inline constexpr int k_tolerance_vs_cpu = 3;

// ── One Dark tokens (ADR-010 §5.1), straight alpha, sRGB-encoded ──

[[nodiscard]] constexpr lx::color tok(int r, int g, int b, float a = 1.f) {
    return lx::color::rgb(static_cast<float>(r) / 255.f, static_cast<float>(g) / 255.f,
                          static_cast<float>(b) / 255.f, a);
}

inline constexpr lx::color k_background = tok(0x3b, 0x41, 0x4d);
inline constexpr lx::color k_surface = tok(0x2f, 0x34, 0x3e);
inline constexpr lx::color k_editor = tok(0x28, 0x2c, 0x33);
inline constexpr lx::color k_element_active = tok(0x45, 0x4a, 0x56);
inline constexpr lx::color k_border = tok(0x46, 0x4b, 0x57);
inline constexpr lx::color k_border_variant = tok(0x36, 0x3c, 0x46);
inline constexpr lx::color k_border_focused = tok(0x47, 0x67, 0x9e);
inline constexpr lx::color k_text = tok(0xdc, 0xe0, 0xe5);
inline constexpr lx::color k_accent = tok(0x74, 0xad, 0xe8);
inline constexpr lx::color k_error = tok(0xd0, 0x72, 0x77);
inline constexpr lx::color k_success = tok(0xa1, 0xc1, 0x81);
inline constexpr lx::color k_warning = tok(0xde, 0xc1, 0x84);

[[nodiscard]] constexpr lx::color with_alpha(lx::color c, float a) {
    c.a = a;
    return c;
}

/// The text caches of the text cases: the bundled fonts plus tests/fonts (Lumen Test Arabic,
/// Lumen Test Emoji), so no glyph comes from a host font. One per process.
struct text_env {
    lx::text::face_cache faces{};
    lx::text::shape_cache shapes{};
    lx::text::itemizer items{};
    bool test_fonts = false;
    text_env() { test_fonts = faces.add_font_dir(LUMEN_TEST_FONT_DIR); }
    [[nodiscard]] lx::text::text_engine engine() { return {&faces, &shapes, &items}; }
};

[[nodiscard]] inline text_env& text_fonts() {
    static const std::unique_ptr<text_env> env = std::make_unique<text_env>();
    return *env;
}

/// The synthetic atlas tiles the sprite cases sample, and the glyph cache the text cases
/// rasterise into the same atlas.
struct tiles {
    lx::gfx::atlas_tile disc{};  ///< R8 16 × 16: an antialiased disc of radius 7
    lx::gfx::atlas_tile ring{};  ///< R8 12 × 12: a 2 px ring
    lx::gfx::atlas_tile ramp{};  ///< R8 32 × 8: coverage 0 → 255 left to right
    lx::gfx::atlas_tile image{}; ///< RGBA8 24 × 24: four premultiplied quadrants, one translucent
    /// Over the atlas `make_tiles` filled (which must outlive it).
    std::shared_ptr<lx::text::glyph_cache> glyphs{};
    /// Lucide icons rasterised into the same atlas (nothing drawn without librsvg).
    std::shared_ptr<lx::ui::icon_cache> icons{};
};

/// Fills `a` with the tiles. Deterministic: the same atlas state every call.
[[nodiscard]] inline bool make_tiles(lx::gfx::atlas& a, tiles& out) {
    using lx::gfx::atlas_format;
    std::uint8_t disc[16 * 16];
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - 8.f;
            const float dy = static_cast<float>(y) + 0.5f - 8.f;
            const float d = std::sqrt(dx * dx + dy * dy) - 7.f;
            const float c = d < -0.5f ? 1.f : (d > 0.5f ? 0.f : 0.5f - d);
            disc[y * 16 + x] = static_cast<std::uint8_t>(c * 255.f + 0.5f);
        }
    std::uint8_t ring[12 * 12];
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 12; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - 6.f;
            const float dy = static_cast<float>(y) + 0.5f - 6.f;
            const float d = std::fabs(std::sqrt(dx * dx + dy * dy) - 4.5f) - 1.f;
            const float c = d < -0.5f ? 1.f : (d > 0.5f ? 0.f : 0.5f - d);
            ring[y * 12 + x] = static_cast<std::uint8_t>(c * 255.f + 0.5f);
        }
    std::uint8_t ramp[32 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 32; ++x)
            ramp[y * 32 + x] = static_cast<std::uint8_t>((x * 255) / 31);
    std::uint8_t image[24 * 24 * 4];
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const lx::color c = x < 12 ? (y < 12 ? k_accent : k_success)
                                       : (y < 12 ? k_error : with_alpha(k_warning, 0.5f));
            std::uint8_t* p = image + (y * 24 + x) * 4;
            p[0] = static_cast<std::uint8_t>(c.r * c.a * 255.f + 0.5f);
            p[1] = static_cast<std::uint8_t>(c.g * c.a * 255.f + 0.5f);
            p[2] = static_cast<std::uint8_t>(c.b * c.a * 255.f + 0.5f);
            p[3] = static_cast<std::uint8_t>(c.a * 255.f + 0.5f);
        }
    const auto put = [&](atlas_format f, unsigned w, unsigned h, const std::uint8_t* px,
                         unsigned stride, lx::gfx::atlas_tile& tile) {
        auto id = a.insert(f, w, h, {px, static_cast<std::size_t>(stride) * h}, stride);
        return id && a.lookup(*id, tile);
    };
    out.glyphs = std::make_shared<lx::text::glyph_cache>(a);
    out.icons = std::make_shared<lx::ui::icon_cache>(a);
    return put(atlas_format::r8, 16, 16, disc, 16, out.disc) &&
           put(atlas_format::r8, 12, 12, ring, 12, out.ring) &&
           put(atlas_format::r8, 32, 8, ramp, 32, out.ramp) &&
           put(atlas_format::rgba8, 24, 24, image, 96, out.image);
}

struct golden_case {
    const char* name = "";
    int width = 0;
    int height = 0;
    lx::color clear{};
    void (*build)(lx::ui::draw_scope& s, const tiles& t) = nullptr;
};

[[nodiscard]] inline lx::ui::quad_style solid(lx::color bg, float radius = 0.f) {
    lx::ui::quad_style q{};
    q.background = bg;
    for (float& r : q.corner_radii)
        r = radius;
    return q;
}

[[nodiscard]] inline lx::ui::quad_style bordered(lx::color bg, lx::color border, float width,
                                                 float radius) {
    lx::ui::quad_style q = solid(bg, radius);
    q.border_color = border;
    for (float& w : q.border_widths)
        w = width;
    return q;
}

[[nodiscard]] inline lx::ui::shadow_style shadow_of(float alpha, float blur, float radius,
                                                    float spread = 0.f, bool inset = false) {
    lx::ui::shadow_style s{};
    s.color = lx::color::rgb(0.f, 0.f, 0.f, alpha);
    for (float& r : s.corner_radii)
        r = radius;
    // GPUI's `BoxShadow::blur_radius` is the Gaussian σ its shader integrates.
    s.blur_sigma = blur;
    s.spread = spread;
    s.inset = inset;
    return s;
}

/// A shadow entry of ADR-010 §5.2, (offset y, blur, alpha), cast by `box`.
inline void cast(lx::ui::draw_scope& s, lx::rect2i box, int dy, float blur, float alpha,
                 float radius) {
    s.shadow({box.x, box.y + dy, box.width, box.height}, shadow_of(alpha, blur, radius));
}

// ── The cases ──

inline void build_quad_radii(lx::ui::draw_scope& s, const tiles&) {
    const float radii[5] = {0.f, 2.f, 6.f, 8.f, 1000.f};
    for (int i = 0; i < 5; ++i)
        s.quad({8 + 44 * i, 10, 36, 28}, solid(k_accent, radii[i]));
}

inline void build_quad_borders(lx::ui::draw_scope& s, const tiles&) {
    for (int i = 0; i < 4; ++i)
        s.quad({8 + 44 * i, 10, 36, 28}, bordered(k_surface, k_border, static_cast<float>(i), 4.f));
    lx::ui::quad_style q = bordered(k_surface, k_accent, 0.f, 6.f);
    q.border_widths[0] = 1.f;
    q.border_widths[1] = 2.f;
    q.border_widths[2] = 3.f;
    q.border_widths[3] = 0.f;
    s.quad({184, 10, 40, 28}, q);
}

inline void build_quad_dashed(lx::ui::draw_scope& s, const tiles&) {
    const float widths[5] = {1.f, 2.f, 3.f, 2.f, 1.f};
    const float radii[5] = {0.f, 0.f, 0.f, 6.f, 1000.f};
    for (int i = 0; i < 5; ++i) {
        lx::ui::quad_style q = bordered(k_editor, k_border_focused, widths[i], radii[i]);
        q.border = lx::ui::border_style::dashed;
        s.quad({6 + 45 * i, 8, 40, 32}, q);
    }
}

inline void build_quad_gradients(lx::ui::draw_scope& s, const tiles&) {
    const float angles[4] = {0.f, 45.f, 90.f, 135.f};
    for (int i = 0; i < 4; ++i) {
        lx::ui::quad_style q = solid(k_accent, 4.f);
        q.background_end = k_error;
        q.gradient_angle_deg = angles[i];
        s.quad({8 + 44 * i, 10, 36, 28}, q);
    }
    // Oklab interpolation of the same stops (gradient_space 1).
    lx::ui::quad_style q = solid(k_accent, 4.f);
    q.background_end = k_error;
    lx::ui::packed_quad_style p = lx::ui::pack_quad_style(q);
    p.gradient_space = 1;
    s.quad({184, 10, 40, 28}, p);
}

/// The five distinct shadow entries of §5.2, each alone under a box of its elevation's
/// radius: elevated (0,2) 3 .12 and (0,1) 0 .06; modal (0,3) 6 .08, (0,6) 12 .04, (0,1) 0 .12.
inline void build_shadow_entries(lx::ui::draw_scope& s, const tiles&) {
    struct entry {
        int dy;
        float blur;
        float alpha;
    };
    const entry entries[5] = {{2, 3.f, 0.12f}, {1, 0.f, 0.06f}, {3, 6.f, 0.08f},
                              {6, 12.f, 0.04f}, {1, 0.f, 0.12f}};
    for (int i = 0; i < 5; ++i)
        cast(s, {24 + 64 * i, 24, 40, 28}, entries[i].dy, entries[i].blur, entries[i].alpha, 6.f);
}

/// Elevated surface (popover) and modal surface on `surface.background`, with their full
/// stacks and the 1 px `border.variant` border.
inline void build_shadow_stacks(lx::ui::draw_scope& s, const tiles&) {
    const lx::rect2i popover{24, 24, 72, 48};
    cast(s, popover, 2, 3.f, 0.12f, 6.f);
    cast(s, popover, 1, 0.f, 0.06f, 6.f);
    s.quad(popover, bordered(k_surface, k_border_variant, 1.f, 6.f));
    const lx::rect2i modal{132, 24, 88, 56};
    cast(s, modal, 2, 3.f, 0.12f, 8.f);
    cast(s, modal, 3, 6.f, 0.08f, 8.f);
    cast(s, modal, 6, 12.f, 0.04f, 8.f);
    cast(s, modal, 1, 0.f, 0.12f, 8.f);
    s.quad(modal, bordered(k_surface, k_border_variant, 1.f, 8.f));
}

/// Strong shadows, where the shape is visible: blurred, spread, hard, inset.
inline void build_shadow_shapes(lx::ui::draw_scope& s, const tiles&) {
    s.shadow({16, 16, 40, 32}, shadow_of(0.6f, 6.f, 8.f));
    s.shadow({84, 20, 32, 24}, shadow_of(0.5f, 3.f, 4.f, 6.f));
    s.shadow({148, 22, 32, 24}, shadow_of(0.7f, 0.f, 6.f));
    s.quad({200, 12, 48, 40}, solid(k_element_active, 8.f));
    s.shadow({200, 12, 48, 40}, shadow_of(0.6f, 4.f, 8.f, 3.f, true));
}

inline void build_sprites(lx::ui::draw_scope& s, const tiles& t) {
    using lx::gfx::primitive_kind;
    // 1:1 on whole pixels (nearest), then scaled 1.5× (bilinear).
    s.sprite(primitive_kind::mono_sprite, t.disc.page, {8, 8, 16, 16}, t.disc.uv(), k_text);
    s.sprite(primitive_kind::mono_sprite, t.disc.page, {32, 8, 24, 24}, t.disc.uv(), k_accent);
    s.sprite(primitive_kind::mono_sprite, t.ring.page, {64, 10, 12, 12}, t.ring.uv(), k_warning);
    // The ramp clipped to its left 20 px by the content mask.
    s.push_clip({84, 0, 20, 64});
    s.sprite(primitive_kind::mono_sprite, t.ramp.page, {84, 12, 32, 8}, t.ramp.uv(), k_text);
    s.pop();
    // A path is an R8 coverage tile too.
    s.sprite(primitive_kind::path, t.ring.page, {84, 28, 12, 12}, t.ring.uv(), k_error);
    // Poly: as is, rounded, grayscale, faded and scaled.
    const lx::color white = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    s.sprite(primitive_kind::poly_sprite, t.image.page, {124, 8, 24, 24}, t.image.uv(), white);
    s.sprite(primitive_kind::poly_sprite, t.image.page, {156, 8, 24, 24}, t.image.uv(), white,
             {6.f, 6.f, 6.f, 6.f});
    // The grayscale flag has no emitter yet: the instance is written by the case below.
    s.sprite(primitive_kind::poly_sprite, t.image.page, {188, 8, 24, 24}, t.image.uv(),
             lx::color::rgb(1.f, 1.f, 1.f, 0.5f));
    s.sprite(primitive_kind::poly_sprite, t.image.page, {216, 4, 36, 36}, t.image.uv(), white,
             {0.f, 10.f, 0.f, 10.f});
}

inline void build_underlines(lx::ui::draw_scope& s, const tiles&) {
    s.quad({0, 0, 200, 56}, solid(k_editor));
    s.underline({8, 10, 80, 1}, k_text, 1.f);
    s.underline({8, 20, 80, 2}, k_accent, 2.f);
    s.underline({104, 8, 88, 3}, k_error, 1.f, true);
    s.underline({104, 18, 88, 6}, k_warning, 2.f, true);
    s.underline({8, 34, 80, 9}, k_success, 3.f, true);
    s.underline({104, 36, 88, 3}, k_text, 1.5f);
}

inline void build_content_mask(lx::ui::draw_scope& s, const tiles&) {
    s.push_clip({20, 12, 64, 40});
    s.quad({8, 4, 48, 32}, bordered(k_accent, k_border_focused, 2.f, 8.f));
    s.shadow({60, 30, 40, 24}, shadow_of(0.6f, 4.f, 6.f));
    s.quad({60, 30, 40, 24}, bordered(k_surface, k_border, 1.f, 6.f));
    s.pop();
    // The mask itself, outlined, to see what was cut.
    lx::ui::quad_style outline = bordered(with_alpha(k_text, 0.f), with_alpha(k_text, 0.25f), 1.f, 0.f);
    s.quad({19, 11, 66, 42}, outline);
}

/// Different kinds overlapping: the order grid puts each above what it overlaps, the
/// batches keep it.
inline void build_mixed_order(lx::ui::draw_scope& s, const tiles& t) {
    using lx::gfx::primitive_kind;
    s.quad({10, 10, 60, 40}, solid(k_success, 6.f));
    s.shadow({40, 24, 60, 36}, shadow_of(0.5f, 4.f, 6.f));
    s.quad({40, 24, 60, 36}, bordered(k_surface, k_border, 1.f, 6.f));
    s.sprite(primitive_kind::mono_sprite, t.disc.page, {52, 30, 16, 16}, t.disc.uv(), k_accent);
    s.underline({48, 48, 40, 2}, k_error, 2.f);
    s.quad({80, 16, 50, 40}, solid(with_alpha(k_warning, 0.5f), 4.f));
    s.sprite(primitive_kind::poly_sprite, t.image.page, {120, 40, 24, 24}, t.image.uv(),
             lx::color::rgb(1.f, 1.f, 1.f, 1.f), {4.f, 4.f, 4.f, 4.f});
    // Disjoint from all the rest: shares an order with the first quad and batches with it.
    s.quad({140, 6, 12, 12}, solid(k_text, 3.f));
}

// ── Text (ADR-010 §8.4): `text_block_node` → mono / poly sprites and underlines ──

/// One unwrapped block of `utf8` at `px` (CSS pixels at scale 1) with its top-left at `at`.
inline void text_line(lx::ui::draw_scope& s, const tiles& t, lx::point2i at, const char* utf8,
                      const char* family, float px, lx::color color, float scale = 1.f,
                      std::span<const lx::text::text_attr> attrs = {},
                      lx::text::glyph_hinting hinting = lx::text::glyph_hinting::none) {
    lx::ui::text_block_props p{};
    p.text = utf8;
    p.font = {family, px * 0.75f}; // points at 96 dpi
    p.color = color;
    p.scale = scale;
    p.wrap = lx::text::text_wrap::none;
    p.attrs = attrs;
    p.hinting = hinting;
    p.context = {text_fonts().engine(), t.glyphs.get()};
    auto node = std::make_unique<lx::ui::text_block_node>();
    node->apply(p);
    const lx::size2i size = node->measure({}, lx::ui::size_hint::preferred);
    node->arrange({at.x, at.y, size.width, size.height});
    node->emit(s);
}

inline constexpr const char* k_arabic_family = "Lumen Test Arabic";
inline constexpr const char* k_emoji_family = "Lumen Test Emoji";

/// 10, 12, 14 and 16 px at scale 1 (the UI sizes of §5.4), then 12 px with the light
/// autohinter a theme may select below scale 1.5.
inline void build_text_sizes(lx::ui::draw_scope& s, const tiles& t) {
    const float sizes[4] = {10.f, 12.f, 14.f, 16.f};
    int y = 4;
    for (float px : sizes) {
        text_line(s, t, {6, y}, "Lumen 0123 quick brown fox", lx::text::k_ui_family, px, k_text);
        y += static_cast<int>(px * 1.5f);
    }
    text_line(s, t, {6, y}, "Lumen 0123 quick brown fox", lx::text::k_ui_family, 12.f, k_text,
              1.f, {}, lx::text::glyph_hinting::light);
}

/// One 14 px block at scales 1, 1.25, 1.5 and 2: glyphs rasterised at size × scale.
inline void build_text_scales(lx::ui::draw_scope& s, const tiles& t) {
    const float scales[4] = {1.f, 1.25f, 1.5f, 2.f};
    int y = 4;
    for (float sc : scales) {
        text_line(s, t, {6, y}, "Hamburgefonstiv 0.125", lx::text::k_ui_family, 14.f, k_accent, sc);
        y += static_cast<int>(14.f * 1.5f * sc);
    }
}

/// Latin and Cyrillic in IBM Plex Sans; Arabic with Latin and digits in Lumen Test Arabic (a
/// right-to-left paragraph: joining forms, bidi reordering).
inline void build_text_scripts(lx::ui::draw_scope& s, const tiles& t) {
    text_line(s, t, {6, 4}, "Latin, \xD0\x9A\xD0\xB8\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBB\xD0\xB8\xD1\x86"
                            "\xD0\xB0: \xD1\x81\xD1\x8A\xD0\xB5\xD1\x88\xD1\x8C \xD0\xB5\xD1\x89"
                            "\xD1\x91",
              lx::text::k_ui_family, 14.f, k_text);
    text_line(s, t, {6, 28},
              "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9: \xD9\x85\xD8\xB1\xD8\xAD"
              "\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 42 Lumen",
              k_arabic_family, 14.f, k_text);
    text_line(s, t, {6, 56}, "Lumen \xD0\x9B\xD1\x8E\xD0\xBC\xD0\xB5\xD0\xBD", lx::text::k_ui_family,
              16.f, k_success);
}

/// Span colours and underlines: a link (accent, straight), a spelling mark (wavy, error), a
/// wavy underline in the span's colour, a straight one in the node's.
inline void build_text_underlines(lx::ui::draw_scope& s, const tiles& t) {
    // "plain link spelling wavy under"
    //  0     6    11       20   25
    static const lx::text::text_attr attrs[] = {
        {6, 10, lx::text::weight::normal, lx::text::style::normal, 0x74ade8ffu,
         lx::text::text_underline::single, lx::text::span_role::link, 1},
        {11, 19, lx::text::weight::normal, lx::text::style::normal, 0,
         lx::text::text_underline::none, lx::text::span_role::spell, 0},
        {20, 24, lx::text::weight::normal, lx::text::style::normal, 0xdec184ffu,
         lx::text::text_underline::wavy, lx::text::span_role::none, 0},
        {25, 30, lx::text::weight::bold, lx::text::style::normal, 0,
         lx::text::text_underline::single, lx::text::span_role::none, 0},
    };
    text_line(s, t, {6, 6}, "plain link spelling wavy under", lx::text::k_ui_family, 14.f, k_text,
              1.f, attrs);
    text_line(s, t, {6, 32}, "plain link spelling wavy under", lx::text::k_ui_family, 14.f, k_text,
              1.5f, attrs);
}

/// Colour glyphs (COLRv0, `poly_sprite`) beside mono text, and faded by the scope opacity.
inline void build_text_emoji(lx::ui::draw_scope& s, const tiles& t) {
    const char* emoji = "\xF0\x9F\x98\x80\xE2\x9D\xA4\xF0\x9F\x91\x8D\xF0\x9F\x9A\x80\xE2\xAD\x90";
    text_line(s, t, {6, 4}, "Lumen", lx::text::k_ui_family, 16.f, k_text);
    text_line(s, t, {60, 4}, emoji, k_emoji_family, 16.f, k_text);
    text_line(s, t, {6, 30}, emoji, k_emoji_family, 24.f, k_text);
    s.push_opacity(0.5f);
    text_line(s, t, {150, 30}, emoji, k_emoji_family, 12.f, k_text, 1.5f);
    s.pop();
}

/// A wrapped paragraph under a content mask that hides its first and last lines: the node
/// skips lines outside the clip, the mask cuts the rest.
inline void build_text_clip(lx::ui::draw_scope& s, const tiles& t) {
    lx::ui::text_block_props p{};
    p.text = "The quick brown fox jumps over the lazy dog. Pack my box with five dozen "
             "liquor jugs. Sphinx of black quartz, judge my vow.";
    p.font = {lx::text::k_ui_family, 12.f * 0.75f};
    p.color = k_text;
    p.context = {text_fonts().engine(), t.glyphs.get()};
    auto node = std::make_unique<lx::ui::text_block_node>();
    node->apply(p);
    const lx::size2i size = node->measure({{0, 0}, {150, 99999}}, lx::ui::size_hint::preferred);
    node->arrange({6, 4, 150, size.height});
    s.push_clip({0, 22, 200, 37});
    node->emit(s);
    s.pop();
    lx::ui::quad_style outline = bordered(with_alpha(k_text, 0.f), with_alpha(k_accent, 0.5f), 1.f, 0.f);
    s.quad({2, 21, 160, 39}, outline);
}

// ── Styled boxes and icons (ADR-010 §4.8, §5.2, §5.5): `styled_node` and `icon_node` under
// the built-in One Dark ──

/// A styled box at `bounds`, laid out and emitted (its children none).
inline void styled_box(lx::ui::draw_scope& s, lx::rect2i bounds, const lx::ui::style& st) {
    auto node = std::make_unique<lx::ui::styled_node>();
    node->apply({st});
    node->arrange(bounds);
    node->emit(s);
}

/// Elevated (popover) and modal surfaces over `surface.background` with their shadow stacks
/// and `border.variant` borders; a translucent box; radii xs, sm, md, lg and full on bordered
/// elements; a focus ring.
inline void build_styled_surfaces(lx::ui::draw_scope& s, const tiles&) {
    using namespace lx::ui;
    lx::ui::set_active_builtin_theme(appearance::dark);
    styled_box(s, {16, 16, 72, 48},
               style{}.bg(lx::ui::tok::elevated_surface_background).rounded(radius::md)
                   .shadow(elevation::elevated_surface));
    styled_box(s, {112, 16, 88, 56},
               style{}.bg(lx::ui::tok::elevated_surface_background).rounded(radius::lg)
                   .shadow(elevation::modal_surface));
    styled_box(s, {224, 16, 60, 40},
               style{}.bg(lx::ui::tok::text_accent).border_1(lx::ui::tok::border_focused).rounded(radius::sm)
                   .opacity(0.5f));
    const radius radii[5] = {radius::xs, radius::sm, radius::md, radius::lg, radius::full};
    for (int i = 0; i < 5; ++i)
        styled_box(s, {16 + 36 * i, 84, 28, 18},
                   style{}.bg(lx::ui::tok::element_background).border_1(lx::ui::tok::border).rounded(radii[i]));
    styled_box(s, {200, 84, 36, 18},
               style{}.bg(lx::ui::tok::ghost_element_hover).border_1(lx::ui::tok::border_focused).rounded(radius::sm));
    styled_box(s, {248, 84, 36, 18}, style{}.bg(lx::ui::tok::element_active).border_b_1(lx::ui::tok::border_variant));
}

/// Lucide icons at Zed's 16, 14 and 12 px in `icon.*` and status tokens, and 16 px at scale
/// 1.25 (a 20 px raster).
inline void build_icons(lx::ui::draw_scope& s, const tiles& t) {
    using namespace lx::ui;
    const icon set[5] = {icon::x, icon::check, icon::settings, icon::bell, icon::chevron_down};
    const lx::ui::tok colors[5] = {lx::ui::tok::icon, lx::ui::tok::icon_muted, lx::ui::tok::icon_accent, lx::ui::tok::error, lx::ui::tok::success};
    const float sizes[3] = {16.f, 14.f, 12.f};
    int y = 6;
    for (float px : sizes) {
        for (int i = 0; i < 5; ++i) {
            auto node = std::make_unique<icon_node>();
            node->apply({set[i], px, 1.f, colors[i], t.icons.get()});
            const lx::size2i sz = node->measure({}, size_hint::preferred);
            node->arrange({6 + 22 * i, y, sz.width, sz.height});
            node->emit(s);
        }
        y += static_cast<int>(px) + 6;
    }
    auto big = std::make_unique<icon_node>();
    big->apply({icon::wifi, 16.f, 1.25f, lx::ui::tok::text, t.icons.get()});
    big->arrange({120, 6, 20, 20});
    big->emit(s);
}

[[nodiscard]] inline std::span<const golden_case> cases() {
    static const std::vector<golden_case> k_cases = [] {
    std::vector<golden_case> v = {
        {"quad_radii", 232, 48, k_editor, build_quad_radii},
        {"quad_borders", 232, 48, k_editor, build_quad_borders},
        {"quad_dashed", 232, 48, k_surface, build_quad_dashed},
        {"quad_gradients", 232, 48, k_editor, build_quad_gradients},
        {"shadow_entries", 344, 80, k_surface, build_shadow_entries},
        {"shadow_stacks", 248, 112, k_surface, build_shadow_stacks},
        {"shadow_shapes", 264, 64, k_background, build_shadow_shapes},
        {"sprites", 264, 48, k_editor, build_sprites},
        {"underlines", 200, 56, k_editor, build_underlines},
        {"content_mask", 112, 64, k_editor, build_content_mask},
        {"mixed_order", 160, 72, k_editor, build_mixed_order},
        {"text_sizes", 232, 104, k_editor, build_text_sizes},
        {"text_scales", 320, 136, k_editor, build_text_scales},
        {"text_scripts", 232, 84, k_editor, build_text_scripts},
        {"text_underlines", 312, 64, k_editor, build_text_underlines},
        {"text_emoji", 264, 72, k_surface, build_text_emoji},
        {"text_clip", 168, 80, k_editor, build_text_clip},
        {"styled_surfaces", 300, 112, k_surface, build_styled_surfaces},
    };
    if (lx::ui::icons_available())
        v.push_back({"icons", 148, 66, k_editor, build_icons});
    else
        std::printf("    SKIP icons: librsvg or the bundled icons are not available\n");
    return v;
    }();
    return k_cases;
}

/// The case's draw list, sorted and batched. The poly sprite at x 188 gets the grayscale
/// flag (no emitter sets it yet).
inline void build(const golden_case& c, const tiles& t, lx::scene::draw_list& out) {
    out.begin({c.width, c.height});
    lx::ui::draw_scope scope{out, {0, 0, c.width, c.height}};
    c.build(scope, t);
    out.batch_for_render();
    for (const lx::gfx::sprite_instance& sp : out.sprite_instances())
        if (sp.bounds[0] == 188.f)
            const_cast<lx::gfx::sprite_instance&>(sp).flags |= 1u;
}

[[nodiscard]] inline lx::gfx::ui_stream stream_of(const lx::scene::draw_list& d) {
    lx::gfx::ui_stream s{};
    s.refs = d.refs();
    s.batches = d.batches();
    s.quads = d.quad_instances();
    s.shadows = d.shadow_instances();
    s.sprites = d.sprite_instances();
    s.underlines = d.underline_instances();
    return s;
}

/// XRGB8888 pixels of one case, rows `width` words.
struct frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> px;

    [[nodiscard]] lx::gfx::pixel_surface surface() {
        lx::gfx::pixel_surface s{};
        s.pixels = reinterpret_cast<unsigned char*>(px.data());
        s.width = static_cast<unsigned>(width);
        s.height = static_cast<unsigned>(height);
        s.stride = static_cast<unsigned>(width) * 4u;
        s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
        return s;
    }
};

[[nodiscard]] inline lx::color premultiplied(lx::color c) {
    return lx::color::rgb(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
}

/// The CPU reference rendering of `c` (whole target damaged). False when the renderer
/// refused.
[[nodiscard]] inline bool render_cpu(const golden_case& c, const tiles& t,
                                     const lx::gfx::atlas& a, frame& out,
                                     bool fast_paths = true) {
    auto list = std::make_unique<lx::scene::draw_list>();
    build(c, t, *list);
    out.width = c.width;
    out.height = c.height;
    out.px.assign(static_cast<std::size_t>(c.width) * c.height, 0u);
    lx::gfx::ui_cpu_renderer r{};
    r.set_fast_paths(fast_paths);
    lx::gfx::damage_region full{};
    full.set_full();
    return static_cast<bool>(r.draw(out.surface(), stream_of(*list), &a, full, premultiplied(c.clear)));
}

// ── Reference files ──

[[nodiscard]] inline std::string reference_path(const char* name) {
    const char* dir = std::getenv("LUMEN_UI_GOLDEN_DIR");
    std::string path = dir && *dir ? dir : LUMEN_UI_GOLDEN_DIR;
    path += '/';
    path += name;
    path += ".png";
    return path;
}

[[nodiscard]] inline bool update_requested() {
    const char* v = std::getenv("LUMEN_UI_GOLDEN_UPDATE");
    return v && std::strcmp(v, "1") == 0;
}

/// 8-bit RGB, rows packed.
struct rgb_image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

[[nodiscard]] inline bool read_png(const std::string& path, rgb_image& out) {
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path.c_str()))
        return false;
    img.format = PNG_FORMAT_RGB;
    out.width = static_cast<int>(img.width);
    out.height = static_cast<int>(img.height);
    out.rgb.assign(PNG_IMAGE_SIZE(img), 0);
    if (!png_image_finish_read(&img, nullptr, out.rgb.data(), 0, nullptr)) {
        png_image_free(&img);
        return false;
    }
    return true;
}

[[nodiscard]] inline bool write_png(const std::string& path, const rgb_image& in) {
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width = static_cast<png_uint_32>(in.width);
    img.height = static_cast<png_uint_32>(in.height);
    img.format = PNG_FORMAT_RGB;
    return png_image_write_to_file(&img, path.c_str(), 0, in.rgb.data(), 0, nullptr) != 0;
}

/// XRGB8888 words (`stride_px` per row) to packed RGB.
[[nodiscard]] inline rgb_image to_rgb(const std::uint32_t* px, int width, int height,
                                      int stride_px) {
    rgb_image out{width, height, {}};
    out.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const std::uint32_t w = px[static_cast<std::size_t>(y) * stride_px + x];
            std::uint8_t* o = out.rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            o[0] = static_cast<std::uint8_t>(w >> 16u);
            o[1] = static_cast<std::uint8_t>(w >> 8u);
            o[2] = static_cast<std::uint8_t>(w);
        }
    return out;
}

struct diff {
    bool reference_found = false;
    bool size_matches = false;
    int max_error = 0;           ///< largest channel difference
    unsigned long long over = 0; ///< pixels with a channel past the tolerance
    int first_x = -1;            ///< the first such pixel
    int first_y = -1;
    [[nodiscard]] bool ok() const { return reference_found && size_matches && over == 0; }
};

/// `px` (XRGB8888, `stride_px` words per row) against the committed reference of `c`:
/// every channel within `tolerance` (0: bit-identical).
[[nodiscard]] inline diff compare_xrgb(const golden_case& c, const std::uint32_t* px,
                                       int stride_px, int tolerance) {
    diff d{};
    rgb_image ref{};
    if (!read_png(reference_path(c.name), ref))
        return d;
    d.reference_found = true;
    d.size_matches = ref.width == c.width && ref.height == c.height;
    if (!d.size_matches)
        return d;
    const rgb_image got = to_rgb(px, c.width, c.height, stride_px);
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x) {
            int worst = 0;
            for (int ch = 0; ch < 3; ++ch) {
                const std::size_t i = (static_cast<std::size_t>(y) * c.width + x) * 3 + ch;
                const int e = std::abs(static_cast<int>(got.rgb[i]) - static_cast<int>(ref.rgb[i]));
                worst = e > worst ? e : worst;
            }
            d.max_error = worst > d.max_error ? worst : d.max_error;
            if (worst > tolerance) {
                if (d.over++ == 0) {
                    d.first_x = x;
                    d.first_y = y;
                }
            }
        }
    return d;
}

} // namespace ui_golden
