module;

#include <stdio.h>

import lx.foundation;

export module lx.gfx:headless;

import std;

export namespace lx::gfx {

struct headless_texture {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<unsigned char> rgba{};
};

/// Software / CI present path — clears a CPU framebuffer and dumps PPM.
class headless_backend {
public:
    [[nodiscard]] lx::result<void> create(unsigned width, unsigned height);
    [[nodiscard]] lx::result<void> begin_frame();
    [[nodiscard]] lx::result<void> clear(lx::color c);
    [[nodiscard]] lx::result<void> blit_texture(unsigned tex_id, lx::rect2i dst);
    /// Source-over fill of `dst` with a premultiplied, encoded colour (a solid draw).
    [[nodiscard]] lx::result<void> fill_solid(lx::rect2i dst, lx::color premultiplied);
    /// Source-over of `premultiplied` × coverage over `dst`, the coverage an R8 tile of
    /// `src_w` × `src_h` texels (`stride` bytes per row) sampled nearest — a `mono_sprite`
    /// (ADR-010 §4.11: server-side titles in the headless dump), clipped to `clip` when it is
    /// not empty.
    [[nodiscard]] lx::result<void> blend_coverage(lx::rect2i dst, lx::rect2i clip,
                                                  const std::uint8_t* coverage, unsigned stride,
                                                  unsigned src_w, unsigned src_h,
                                                  lx::color premultiplied);
    /// Register or replace a CPU texture for software blit (tests / headless clients).
    void register_texture(unsigned tex_id, unsigned width, unsigned height,
                          const unsigned char* rgba);
    /// Drops a texture's pixels. The table is keyed by texture id and ids never repeat, so
    /// without this every buffer ever registered stays resident.
    void forget_texture(unsigned tex_id);
    [[nodiscard]] lx::result<void> end_frame_and_dump(const char* path);
    [[nodiscard]] double last_frame_ms() const;

    [[nodiscard]] unsigned width() const;
    [[nodiscard]] unsigned height() const;
    [[nodiscard]] const unsigned char* pixel_data() const;

private:
    void blit_rgba_nearest(const headless_texture& tex, lx::rect2i dst);

    unsigned width_ = 0;
    unsigned height_ = 0;
    std::vector<unsigned char> pixels_{};
    std::unordered_map<unsigned, headless_texture> textures_{};
    double last_frame_ms_ = 0.0;
    std::chrono::steady_clock::time_point frame_start_{};
    bool in_frame_ = false;
};

} // namespace lx::gfx


lx::result<void> lx::gfx::headless_backend::create(unsigned width, unsigned height) {
    if (width == 0 || height == 0) {
        return lx::invalid_argument("headless_backend::create requires non-zero size");
    }
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<std::size_t>(width) * height * 3u, 0);
    last_frame_ms_ = 0.0;
    in_frame_ = false;
    return {};
}

lx::result<void> lx::gfx::headless_backend::begin_frame() {
    if (width_ == 0 || height_ == 0) {
        return lx::invalid_argument("headless_backend not created");
    }
    frame_start_ = std::chrono::steady_clock::now();
    in_frame_ = true;
    return {};
}

lx::result<void> lx::gfx::headless_backend::clear(lx::color c) {
    if (!in_frame_) {
        return lx::invalid_argument("clear outside begin_frame");
    }
    const auto r = static_cast<unsigned char>(c.r * 255.f);
    const auto g = static_cast<unsigned char>(c.g * 255.f);
    const auto b = static_cast<unsigned char>(c.b * 255.f);
    for (std::size_t i = 0; i + 2 < pixels_.size(); i += 3) {
        pixels_[i] = r;
        pixels_[i + 1] = g;
        pixels_[i + 2] = b;
    }
    return {};
}

void lx::gfx::headless_backend::register_texture(unsigned tex_id, unsigned width,
                                                  unsigned height,
                                                  const unsigned char* rgba) {
    if (!rgba || width == 0 || height == 0)
        return;
    headless_texture tex{};
    tex.width = width;
    tex.height = height;
    tex.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4u);
    textures_[tex_id] = std::move(tex);
}

void lx::gfx::headless_backend::forget_texture(unsigned tex_id) { textures_.erase(tex_id); }

void lx::gfx::headless_backend::blit_rgba_nearest(const headless_texture& tex, lx::rect2i dst) {
    if (tex.rgba.empty() || dst.width <= 0 || dst.height <= 0)
        return;

    const int x0 = dst.x < 0 ? 0 : dst.x;
    const int y0 = dst.y < 0 ? 0 : dst.y;
    const int x1 = dst.x + dst.width > static_cast<int>(width_) ? static_cast<int>(width_)
                                                                : dst.x + dst.width;
    const int y1 = dst.y + dst.height > static_cast<int>(height_) ? static_cast<int>(height_)
                                                                   : dst.y + dst.height;

    for (int y = y0; y < y1; ++y) {
        const int sy = ((y - dst.y) * static_cast<int>(tex.height)) / dst.height;
        for (int x = x0; x < x1; ++x) {
            const int sx = ((x - dst.x) * static_cast<int>(tex.width)) / dst.width;
            const std::size_t src = (static_cast<std::size_t>(sy) * tex.width +
                                     static_cast<std::size_t>(sx)) *
                                    4u;
            if (src + 3 >= tex.rgba.size())
                continue;
            const float a = static_cast<float>(tex.rgba[src + 3]) / 255.f;
            const std::size_t dst_idx =
                (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 3u;
            if (dst_idx + 2 >= pixels_.size())
                continue;
            const float inv = 1.f - a;
            pixels_[dst_idx] = static_cast<unsigned char>(
                tex.rgba[src] * a + pixels_[dst_idx] * inv);
            pixels_[dst_idx + 1] = static_cast<unsigned char>(
                tex.rgba[src + 1] * a + pixels_[dst_idx + 1] * inv);
            pixels_[dst_idx + 2] = static_cast<unsigned char>(
                tex.rgba[src + 2] * a + pixels_[dst_idx + 2] * inv);
        }
    }
}

lx::result<void> lx::gfx::headless_backend::blit_texture(unsigned tex_id, lx::rect2i dst) {
    if (!in_frame_) {
        return lx::invalid_argument("blit outside begin_frame");
    }
    const auto it = textures_.find(tex_id);
    if (it == textures_.end()) {
        // No placeholder fill: a stand-in rect makes a golden-image test pass while
        // proving nothing about the client's actual buffer contents.
        return lx::invalid_argument("blit of unregistered texture");
    }
    blit_rgba_nearest(it->second, dst);
    return {};
}

lx::result<void> lx::gfx::headless_backend::fill_solid(lx::rect2i dst, lx::color c) {
    if (!in_frame_) {
        return lx::invalid_argument("fill outside begin_frame");
    }
    const int x0 = dst.x > 0 ? dst.x : 0;
    const int y0 = dst.y > 0 ? dst.y : 0;
    const int x1 = dst.x + dst.width < static_cast<int>(width_) ? dst.x + dst.width
                                                                : static_cast<int>(width_);
    const int y1 = dst.y + dst.height < static_cast<int>(height_) ? dst.y + dst.height
                                                                  : static_cast<int>(height_);
    const float inv = 1.f - (c.a < 0.f ? 0.f : (c.a > 1.f ? 1.f : c.a));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 3u;
            pixels_[i] = static_cast<unsigned char>(c.r * 255.f + pixels_[i] * inv);
            pixels_[i + 1] = static_cast<unsigned char>(c.g * 255.f + pixels_[i + 1] * inv);
            pixels_[i + 2] = static_cast<unsigned char>(c.b * 255.f + pixels_[i + 2] * inv);
        }
    }
    return {};
}

lx::result<void> lx::gfx::headless_backend::blend_coverage(lx::rect2i dst, lx::rect2i clip,
                                                          const std::uint8_t* coverage,
                                                          unsigned stride, unsigned src_w,
                                                          unsigned src_h, lx::color c) {
    if (!in_frame_)
        return lx::invalid_argument("blend outside begin_frame");
    if (!coverage || dst.width <= 0 || dst.height <= 0 || src_w == 0 || src_h == 0)
        return {};
    int x0 = dst.x > 0 ? dst.x : 0;
    int y0 = dst.y > 0 ? dst.y : 0;
    int x1 = dst.x + dst.width < static_cast<int>(width_) ? dst.x + dst.width
                                                          : static_cast<int>(width_);
    int y1 = dst.y + dst.height < static_cast<int>(height_) ? dst.y + dst.height
                                                            : static_cast<int>(height_);
    if (clip.width > 0 && clip.height > 0) {
        x0 = x0 > clip.x ? x0 : clip.x;
        y0 = y0 > clip.y ? y0 : clip.y;
        x1 = x1 < clip.x + clip.width ? x1 : clip.x + clip.width;
        y1 = y1 < clip.y + clip.height ? y1 : clip.y + clip.height;
    }
    for (int y = y0; y < y1; ++y) {
        const unsigned sy = static_cast<unsigned>(((y - dst.y) * static_cast<int>(src_h)) / dst.height);
        for (int x = x0; x < x1; ++x) {
            const unsigned sx =
                static_cast<unsigned>(((x - dst.x) * static_cast<int>(src_w)) / dst.width);
            const float k = static_cast<float>(coverage[sy * stride + sx]) / 255.f;
            if (k <= 0.f)
                continue;
            const float inv = 1.f - c.a * k;
            const std::size_t i =
                (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 3u;
            pixels_[i] = static_cast<unsigned char>(c.r * k * 255.f + pixels_[i] * inv);
            pixels_[i + 1] = static_cast<unsigned char>(c.g * k * 255.f + pixels_[i + 1] * inv);
            pixels_[i + 2] = static_cast<unsigned char>(c.b * k * 255.f + pixels_[i + 2] * inv);
        }
    }
    return {};
}

lx::result<void> lx::gfx::headless_backend::end_frame_and_dump(const char* path) {
    if (!in_frame_) {
        return lx::invalid_argument("end_frame outside begin_frame");
    }
    in_frame_ = false;
    last_frame_ms_ = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - frame_start_)
                         .count();

    if (!path || path[0] == '\0')
        return {};

    FILE* f = std::fopen(path, "wb");
    if (!f) {
        return lx::make_error(lx::error_domain::io, 0, "failed to open dump path");
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width_, height_);
    const auto written = std::fwrite(pixels_.data(), 1, pixels_.size(), f);
    std::fclose(f);
    if (written != pixels_.size()) {
        return lx::make_error(lx::error_domain::io, 0, "failed to write PPM");
    }
    return {};
}

double lx::gfx::headless_backend::last_frame_ms() const { return last_frame_ms_; }
unsigned lx::gfx::headless_backend::width() const { return width_; }
unsigned lx::gfx::headless_backend::height() const { return height_; }
const unsigned char* lx::gfx::headless_backend::pixel_data() const { return pixels_.data(); }
