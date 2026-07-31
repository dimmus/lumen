module;

#if defined(LUMEN_HAS_TEXT)
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>
#include <mutex>
#endif

import lx.foundation;
import lx.gfx;

export module lx.text;

export namespace lx::text {

enum class weight { normal, bold };
enum class style { normal, italic };

struct font_desc {
    const char* family = "Sans";
    float size_pt = 11.f;
    weight weight = weight::normal;
    style style = style::normal;
};

struct glyph_run {
    const char* utf8 = "";
    float width = 0.f;
    float height = 0.f;
};

class font_stack {
public:
    /// Process-wide shaping cache — face loading is expensive, share it.
    [[nodiscard]] static font_stack& shared();

    void load_default();
    void load(font_desc desc);
    [[nodiscard]] glyph_run shape(const char* utf8, font_desc desc) const;
    [[nodiscard]] float measure_width(const char* utf8, font_desc desc) const;

    /// True when a real font face backs shaping; false means metric estimation.
    [[nodiscard]] bool has_face() const;

private:
    gfx::glyph_atlas atlas_{};
};

class text_layout {
public:
    void set_text(const char* utf8);
    void set_font(font_desc desc);
    void set_max_width(float width);
    [[nodiscard]] lx::size2i bounding_size() const;
    void paint(lx::gfx::render_pass& pass, lx::point2i origin, lx::color color) const;

private:
    void reshape();

    const char* text_ = "";
    font_desc font_{};
    float max_width_ = 0.f;
    glyph_run shaped_{};
};

} // namespace lx::text


#if defined(LUMEN_HAS_TEXT)
namespace lx::text::detail {

/// Owns FreeType + the resolved hb_font_t. Shaping without a face is never attempted:
/// hb_shape() dereferences its font argument, so a null face must fall back to metrics.
class face_cache {
public:
    static face_cache& instance() {
        static face_cache c{};
        return c;
    }

    /// Function-local static: runs at exit, so FreeType/HarfBuzz handles and the
    /// fontconfig global config are released rather than reported as leaks.
    ~face_cache() {
        std::lock_guard lock{mutex_};
        release_locked();
        if (library_) {
            FT_Done_FreeType(library_);
            library_ = nullptr;
        }
        if (init_attempted_ && fontconfig_ready_)
            FcFini();
    }

    face_cache(const face_cache&) = delete;
    face_cache& operator=(const face_cache&) = delete;

    hb_font_t* acquire(const font_desc& desc) {
        std::lock_guard lock{mutex_};
        if (!init_attempted_) {
            init_attempted_ = true;
            if (FT_Init_FreeType(&library_) != 0)
                library_ = nullptr;
            if (!FcInit())
                fontconfig_ready_ = false;
        }
        if (!library_ || !fontconfig_ready_)
            return nullptr;

        const int px = static_cast<int>(desc.size_pt * 64.f);
        if (hb_font_ && cached_px_ == px && cached_family_ == desc.family &&
            cached_weight_ == desc.weight && cached_style_ == desc.style)
            return hb_font_;

        release_locked();

        FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(
            desc.family && desc.family[0] ? desc.family : "Sans"));
        if (!pattern)
            return nullptr;
        FcPatternAddInteger(pattern, FC_WEIGHT,
                            desc.weight == weight::bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
        FcPatternAddInteger(pattern, FC_SLANT,
                            desc.style == style::italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);

        FcResult match_result{};
        FcPattern* matched = FcFontMatch(nullptr, pattern, &match_result);
        FcPatternDestroy(pattern);
        if (!matched || match_result != FcResultMatch) {
            if (matched) FcPatternDestroy(matched);
            return nullptr;
        }

        FcChar8* file = nullptr;
        int face_index = 0;
        if (FcPatternGetString(matched, FC_FILE, 0, &file) != FcResultMatch || !file) {
            FcPatternDestroy(matched);
            return nullptr;
        }
        FcPatternGetInteger(matched, FC_INDEX, 0, &face_index);

        FT_Face face = nullptr;
        const FT_Error err =
            FT_New_Face(library_, reinterpret_cast<const char*>(file), face_index, &face);
        FcPatternDestroy(matched);
        if (err != 0 || !face)
            return nullptr;

        FT_Set_Char_Size(face, 0, px, 0, 96);
        hb_font_t* font = hb_ft_font_create_referenced(face);
        if (!font) {
            FT_Done_Face(face);
            return nullptr;
        }

        ft_face_ = face;
        hb_font_ = font;
        cached_px_ = px;
        cached_family_ = desc.family;
        cached_weight_ = desc.weight;
        cached_style_ = desc.style;
        return hb_font_;
    }

private:
    face_cache() = default;

    void release_locked() {
        if (hb_font_) {
            hb_font_destroy(hb_font_);
            hb_font_ = nullptr;
        }
        if (ft_face_) {
            FT_Done_Face(ft_face_);
            ft_face_ = nullptr;
        }
    }

    std::mutex mutex_{};
    FT_Library library_ = nullptr;
    FT_Face ft_face_ = nullptr;
    hb_font_t* hb_font_ = nullptr;
    int cached_px_ = 0;
    const char* cached_family_ = nullptr;
    weight cached_weight_ = weight::normal;
    style cached_style_ = style::normal;
    bool init_attempted_ = false;
    bool fontconfig_ready_ = true;
};

} // namespace lx::text::detail
#endif

namespace lx::text::detail {

/// Monospace-ish approximation used when no font face is available (headless CI).
[[nodiscard]] inline glyph_run estimate(const char* utf8, const font_desc& desc) {
    float w = 0.f;
    for (const char* p = utf8; *p; ++p) {
        // Count UTF-8 lead bytes only, so multibyte text is not over-measured.
        if ((static_cast<unsigned char>(*p) & 0xC0u) != 0x80u)
            w += desc.size_pt * 0.55f;
    }
    return {utf8, w, desc.size_pt * 1.2f};
}

} // namespace lx::text::detail


lx::text::font_stack& lx::text::font_stack::shared() {
    static font_stack instance{};
    return instance;
}

void lx::text::font_stack::load_default() { load({}); }

void lx::text::font_stack::load(font_desc desc) {
#if defined(LUMEN_HAS_TEXT)
    (void)detail::face_cache::instance().acquire(desc);
#else
    (void)desc;
#endif
}

bool lx::text::font_stack::has_face() const {
#if defined(LUMEN_HAS_TEXT)
    return detail::face_cache::instance().acquire(font_desc{}) != nullptr;
#else
    return false;
#endif
}

lx::text::glyph_run lx::text::font_stack::shape(const char* utf8, font_desc desc) const {
    if (!utf8 || utf8[0] == '\0')
        return {};
#if defined(LUMEN_HAS_TEXT)
    hb_font_t* font = detail::face_cache::instance().acquire(desc);
    if (!font)
        return detail::estimate(utf8, desc);

    hb_buffer_t* buf = hb_buffer_create();
    if (!buf)
        return detail::estimate(utf8, desc);
    hb_buffer_add_utf8(buf, utf8, -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(font, buf, nullptr, 0);

    unsigned count = 0;
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
    float width = 0.f;
    for (unsigned i = 0; i < count && pos; ++i)
        width += static_cast<float>(pos[i].x_advance) / 64.f;
    hb_buffer_destroy(buf);

    return {utf8, width, desc.size_pt * 1.2f};
#else
    return detail::estimate(utf8, desc);
#endif
}

float lx::text::font_stack::measure_width(const char* utf8, font_desc desc) const {
    return shape(utf8, desc).width;
}

void lx::text::text_layout::reshape() {
    shaped_ = font_stack::shared().shape(text_, font_);
}

void lx::text::text_layout::set_text(const char* t) {
    text_ = t ? t : "";
    reshape();
}

void lx::text::text_layout::set_font(font_desc d) {
    font_ = d;
    reshape();
}

void lx::text::text_layout::set_max_width(float w) { max_width_ = w; }

lx::size2i lx::text::text_layout::bounding_size() const {
    const float height = shaped_.height > 0.f ? shaped_.height : font_.size_pt * 1.2f;
    float width = shaped_.width;
    if (max_width_ > 0.f && width > max_width_)
        width = max_width_;
    return {static_cast<int>(width), static_cast<int>(height)};
}

void lx::text::text_layout::paint(lx::gfx::render_pass& pass, lx::point2i origin,
                                  lx::color color) const {
    (void)pass;
    (void)origin;
    (void)color;
}
