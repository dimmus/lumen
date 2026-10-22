module;
// The desktop background (lumen-shell): one wlr-layer-shell surface per output on the
// background layer, under every window and the panel.
//
// What is drawn is the shell's policy (`--background`, `LUMEN_BACKGROUND`); the compositor
// only composites a surface, as it would swaybg's. What it costs is chosen per kind:
//
//   - a colour is a wp_single_pixel_buffer_v1 shown at the output's size by a wp_viewport:
//     no pixels at all, and the compositor draws it as a solid fill, the price of a clear;
//   - everything else is rendered once per output size and scale, at the output's pixel
//     size (fractional scale), on a worker thread so the panel never waits: the built-in
//     default (`lumen`, procedural — no file to read at start-up), two-stop gradients
//     (interpolated in linear light), and images — PNG (libpng), JPEG (libjpeg-turbo,
//     decoded at 1/2, 1/4 or 1/8 by its DCT when far larger than the output), WebP
//     (libwebp) and SVG (librsvg, rendered straight at the size shown, so always sharp) —
//     filled, fitted, centred, stretched or tiled.
//
// Smooth ramps are dithered (interleaved gradient noise, ±½ LSB) so 8-bit outputs show no
// bands. The surface is opaque (no blending, and the compositor skips whatever windows
// cover) and takes no input. Its one shm buffer is unmapped here as soon as it is committed
// and destroyed when the compositor releases it: once on screen the background holds no
// memory in the shell, only the compositor's copy of it.
//
// Rendering and decoding are pure functions over caller memory, safe on any thread; the
// Wayland objects are made and used on the shell's thread only.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <string.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#if defined(LUMEN_PROTOCOL_VIEWPORTER) && defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1)
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#define LUMEN_BG_FRACTIONAL 1
#if defined(LUMEN_PROTOCOL_SINGLE_PIXEL_BUFFER_V1)
#include "single-pixel-buffer-v1-client-protocol.h"
#define LUMEN_BG_SINGLE_PIXEL 1
#endif
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

#if defined(LUMEN_HAS_PNG)
#include <png.h>
#endif
#if defined(LUMEN_HAS_JPEG)
#include <turbojpeg.h>
#endif
#if defined(LUMEN_HAS_WEBP)
#include <webp/decode.h>
#endif
#if defined(LUMEN_HAS_RSVG)
#include <cairo.h>
#include <librsvg/rsvg.h>
#endif

import lx.foundation;
import lx.trace;

export module lx.shell:background;

import std;

export namespace lx::shell {

enum class background_kind : unsigned char { lumen, color, gradient, image };
/// How an image covers the output: `fill` scales to cover and crops (the default),
/// `fit` scales to fit inside over the colour, `center` shows it 1:1, `stretch` ignores its
/// aspect, `tile` repeats it 1:1 from the top-left corner.
enum class background_fit : unsigned char { fill, fit, center, stretch, tile };

struct background_spec {
    background_kind kind = background_kind::lumen;
    /// The colour; a gradient's first stop; what `fit` and `center` leave uncovered.
    std::uint32_t color = 0xFF10182Au;
    /// A gradient's second stop.
    std::uint32_t color2 = 0xFF22143Au;
    /// A gradient's direction in degrees: 0 left to right, 90 top to bottom.
    float angle_deg = 90.f;
    background_fit fit = background_fit::fill;
    std::string path{};
};

/// `--background` / `LUMEN_BACKGROUND`:
///   `lumen`                              the built-in default
///   `#RRGGBB` | `color:#RRGGBB`          a colour
///   `gradient:#RRGGBB:#RRGGBB[:DEG]`     two stops, 90° (top to bottom) unless given
///   `image:PATH[:fill|fit|center|stretch|tile]` or an absolute PATH
/// Nothing (`std::nullopt`) for anything else.
[[nodiscard]] std::optional<background_spec> parse_background(std::string_view text);

/// Renderers into `px`: `w`×`h` XRGB8888 pixels, `stride` pixels per row. Pure; any thread.
void render_lumen(std::uint32_t* px, int w, int h, int stride);
void render_gradient(std::uint32_t* px, int w, int h, int stride, std::uint32_t from,
                     std::uint32_t to, float angle_deg);
void render_color(std::uint32_t* px, int w, int h, int stride, std::uint32_t color);
/// Places premultiplied ARGB `src` (`sw`×`sh`) per `fit`, composited over `under`.
void render_image(std::uint32_t* px, int w, int h, int stride, const std::uint32_t* src, int sw,
                  int sh, background_fit fit, std::uint32_t under);
/// Decodes `path` (PNG, JPEG, WebP or SVG, by content) into premultiplied ARGB for a
/// `w`×`h` output shown with `fit`: a JPEG far larger than it is DCT-scaled while decoding,
/// an SVG is rendered at the size it will be shown. False when it cannot be read.
[[nodiscard]] bool decode_background(const std::string& path, int w, int h, background_fit fit,
                                     std::vector<std::uint32_t>& out, int& out_w, int& out_h);
/// The image decoders built in.
struct background_decoders {
    bool png = false, jpeg = false, webp = false, svg = false;
};
[[nodiscard]] background_decoders background_formats();
/// The whole render of `spec` at `w`×`h`. An image that cannot be read falls back to the
/// built-in default; false then.
bool render_background(const background_spec& spec, std::uint32_t* px, int w, int h, int stride);

struct background_globals {
    void* compositor = nullptr;   // wl_compositor*
    void* shm = nullptr;          // wl_shm*
    void* layer_shell = nullptr;  // zwlr_layer_shell_v1*
    void* viewporter = nullptr;   // wp_viewporter*
    void* fractional = nullptr;   // wp_fractional_scale_manager_v1*
    void* single_pixel = nullptr; // wp_single_pixel_buffer_manager_v1*
};

/// One output's background surface. Shell thread only, except the render job it starts.
class background {
public:
    background() = default;
    ~background();
    background(const background&) = delete;
    background& operator=(const background&) = delete;

    /// `spec` must outlive this. `notify_fd` is an eventfd the shell polls: a finished
    /// render writes to it, and the shell then calls `after_dispatch`.
    [[nodiscard]] lx::result<void> create(background_globals globals, const background_spec* spec,
                                          void* output, int notify_fd);
    void destroy();
    /// After every dispatch: starts a render when the size or scale changed, and shows a
    /// finished one.
    void after_dispatch();
    /// The compositor closed the surface (its output went).
    [[nodiscard]] bool closed() const { return closed_; }
    /// A frame is on screen.
    [[nodiscard]] bool shown() const { return shown_; }

    struct events;

private:
    struct job {
        std::thread thread;
        std::atomic<bool> done{false};
        bool ok = true;
        double ms = 0; // the render's own time
        void* buffer = nullptr; // wl_buffer*
        void* data = nullptr;   // the mapping the worker renders into
        std::size_t size = 0;
        int pw = 0;
        int ph = 0;
    };
    void start_render();
    void finish_render();
    void show_color();
    void cancel_job();

    background_globals g_{};
    const background_spec* spec_ = nullptr;
    int notify_fd_ = -1;
    void* surface_ = nullptr;    // wl_surface*
    void* layer_ = nullptr;      // zwlr_layer_surface_v1*
    void* viewport_ = nullptr;   // wp_viewport*
    void* fractional_ = nullptr; // wp_fractional_scale_v1*
    unsigned scale120_ = 120;
    int width_ = 0; // logical
    int height_ = 0;
    int shown_pw_ = 0;
    int shown_ph_ = 0;
    bool configured_ = false;
    bool must_commit_ = false;
    bool closed_ = false;
    bool shown_ = false;
    std::unique_ptr<job> job_{};
};

} // namespace lx::shell

namespace {

constexpr const char* k_bg_category = "shell.background";

// ── colour ────────────────────────────────────────────────────────────────────

struct rgbf {
    float r, g, b;
};

rgbf unpack_rgb(std::uint32_t c) {
    return {static_cast<float>((c >> 16) & 0xFFu) / 255.f, static_cast<float>((c >> 8) & 0xFFu) / 255.f,
            static_cast<float>(c & 0xFFu) / 255.f};
}

float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float v) {
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

/// Interleaved gradient noise (Jimenez 2014) in [-0.5, 0.5): a dither with no visible
/// pattern, cheaper than blue noise and free of a texture.
inline float dither_ign(int x, int y) {
    const float f = 0.06711056f * static_cast<float>(x) + 0.00583715f * static_cast<float>(y);
    const float g = 52.9829189f * (f - std::floor(f));
    return (g - std::floor(g)) - 0.5f;
}

/// sRGB channel values in 0..1, dithered by `n` (in LSBs), to XRGB8888.
inline std::uint32_t pack_xrgb(float r, float g, float b, float n) {
    const auto q = [n](float v) {
        const float s = v * 255.f + 0.5f + n;
        return static_cast<std::uint32_t>(s < 0.f ? 0.f : (s > 255.f ? 255.f : s));
    };
    return 0xFF000000u | (q(r) << 16) | (q(g) << 8) | q(b);
}

bool parse_hex(std::string_view s, std::uint32_t& out) {
    if (!s.empty() && s.front() == '#')
        s.remove_prefix(1);
    if (s.size() != 6)
        return false;
    std::uint32_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= static_cast<std::uint32_t>(c - 'A' + 10);
        else
            return false;
    }
    out = 0xFF000000u | v;
    return true;
}

// ── resampling ────────────────────────────────────────────────────────────────

/// `src` (`sw`×`sh`, premultiplied ARGB) to `dw`×`dh`: area averaging on each axis that
/// shrinks, bilinear on each that grows. Separable: a horizontal pass into a float row
/// buffer, then a vertical one.
void resample_area_bilinear(const std::uint32_t* src, int sw, int sh, std::uint32_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    // Horizontal pass, row by row: sh rows of dw pixels, 4 floats each.
    std::vector<float> mid(static_cast<std::size_t>(dw) * sh * 4);
    const double sx = static_cast<double>(sw) / dw;
    for (int y = 0; y < sh; ++y) {
        const std::uint32_t* row = src + static_cast<std::size_t>(y) * sw;
        float* out = mid.data() + static_cast<std::size_t>(y) * dw * 4;
        for (int x = 0; x < dw; ++x) {
            double a = 0, r = 0, g = 0, b = 0;
            if (sx > 1.0) {
                const double x0 = x * sx, x1 = x0 + sx;
                for (int i = static_cast<int>(x0); i < sw && i < x1; ++i) {
                    const double w = std::min<double>(i + 1, x1) - std::max<double>(i, x0);
                    const std::uint32_t p = row[i];
                    a += w * (p >> 24);
                    r += w * ((p >> 16) & 0xFFu);
                    g += w * ((p >> 8) & 0xFFu);
                    b += w * (p & 0xFFu);
                }
                a /= sx;
                r /= sx;
                g /= sx;
                b /= sx;
            } else {
                double fx = (x + 0.5) * sx - 0.5;
                fx = fx < 0 ? 0 : fx;
                const int i0 = std::min(static_cast<int>(fx), sw - 1);
                const int i1 = std::min(i0 + 1, sw - 1);
                const double t = fx - i0;
                const std::uint32_t p0 = row[i0], p1 = row[i1];
                a = (1 - t) * (p0 >> 24) + t * (p1 >> 24);
                r = (1 - t) * ((p0 >> 16) & 0xFFu) + t * ((p1 >> 16) & 0xFFu);
                g = (1 - t) * ((p0 >> 8) & 0xFFu) + t * ((p1 >> 8) & 0xFFu);
                b = (1 - t) * (p0 & 0xFFu) + t * (p1 & 0xFFu);
            }
            out[x * 4 + 0] = static_cast<float>(a);
            out[x * 4 + 1] = static_cast<float>(r);
            out[x * 4 + 2] = static_cast<float>(g);
            out[x * 4 + 3] = static_cast<float>(b);
        }
    }
    // Vertical pass.
    const double sy = static_cast<double>(sh) / dh;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            if (sy > 1.0) {
                const double y0 = y * sy, y1 = y0 + sy;
                for (int j = static_cast<int>(y0); j < sh && j < y1; ++j) {
                    const float w = static_cast<float>(std::min<double>(j + 1, y1) - std::max<double>(j, y0));
                    const float* m = mid.data() + (static_cast<std::size_t>(j) * dw + x) * 4;
                    for (int c = 0; c < 4; ++c)
                        acc[c] += w * m[c];
                }
                for (float& v : acc)
                    v /= static_cast<float>(sy);
            } else {
                double fy = (y + 0.5) * sy - 0.5;
                fy = fy < 0 ? 0 : fy;
                const int j0 = std::min(static_cast<int>(fy), sh - 1);
                const int j1 = std::min(j0 + 1, sh - 1);
                const float t = static_cast<float>(fy - j0);
                const float* m0 = mid.data() + (static_cast<std::size_t>(j0) * dw + x) * 4;
                const float* m1 = mid.data() + (static_cast<std::size_t>(j1) * dw + x) * 4;
                for (int c = 0; c < 4; ++c)
                    acc[c] = (1 - t) * m0[c] + t * m1[c];
            }
            const auto q = [](float v) {
                return static_cast<std::uint32_t>(v < 0.f ? 0.f : (v > 255.f ? 255.f : v + 0.5f));
            };
            dst[static_cast<std::size_t>(y) * dw + x] =
                (q(acc[0]) << 24) | (q(acc[1]) << 16) | (q(acc[2]) << 8) | q(acc[3]);
        }
    }
}

/// Premultiplied `s` over opaque `d`.
inline std::uint32_t blend_over(std::uint32_t s, std::uint32_t d) {
    const unsigned a = s >> 24;
    if (a == 255)
        return s;
    const unsigned ia = 255 - a;
    const auto ch = [&](unsigned shift) {
        const unsigned v = ((s >> shift) & 0xFFu) + (((d >> shift) & 0xFFu) * ia + 127) / 255;
        return (v > 255 ? 255u : v) << shift;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

// ── decoding ──────────────────────────────────────────────────────────────────

constexpr std::size_t k_max_pixels = std::size_t{1} << 28; // 16384² — bounds a hostile file

bool read_whole_file(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const auto n = f.tellg();
    if (n <= 0 || n > (std::streamoff{1} << 30))
        return false;
    f.seekg(0);
    out.resize(static_cast<std::size_t>(n));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), n));
}

void premultiply_all(std::vector<std::uint32_t>& px) {
    for (auto& p : px) {
        const unsigned a = p >> 24;
        if (a == 255)
            continue;
        const unsigned r = (((p >> 16) & 0xFFu) * a + 127) / 255u;
        const unsigned g = (((p >> 8) & 0xFFu) * a + 127) / 255u;
        const unsigned b = ((p & 0xFFu) * a + 127) / 255u;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/// The size an image of `iw`×`ih` is shown at on a `w`×`h` output under `fit`.
void fit_size(int iw, int ih, int w, int h, lx::shell::background_fit fit, int& sw, int& sh) {
    using lx::shell::background_fit;
    switch (fit) {
    case background_fit::fill:
    case background_fit::fit: {
        const double sx = static_cast<double>(w) / iw, sy = static_cast<double>(h) / ih;
        const double s = fit == background_fit::fill ? std::max(sx, sy) : std::min(sx, sy);
        sw = std::max(1, static_cast<int>(std::lround(iw * s)));
        sh = std::max(1, static_cast<int>(std::lround(ih * s)));
        return;
    }
    case background_fit::stretch:
        sw = w;
        sh = h;
        return;
    case background_fit::center:
    case background_fit::tile:
        sw = iw;
        sh = ih;
        return;
    }
}

#if defined(LUMEN_HAS_PNG)
bool decode_png(const std::vector<unsigned char>& file, std::vector<std::uint32_t>& out, int& w, int& h) {
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, file.data(), file.size()))
        return false;
    img.format = PNG_FORMAT_BGRA; // 0xAARRGGBB in a little-endian word
    if (img.width == 0 || img.height == 0 ||
        static_cast<std::size_t>(img.width) * img.height > k_max_pixels) {
        png_image_free(&img);
        return false;
    }
    out.resize(static_cast<std::size_t>(img.width) * img.height);
    if (!png_image_finish_read(&img, nullptr, out.data(), 0, nullptr))
        return false;
    w = static_cast<int>(img.width);
    h = static_cast<int>(img.height);
    premultiply_all(out);
    return true;
}
#endif

#if defined(LUMEN_HAS_JPEG)
bool decode_jpeg(const std::vector<unsigned char>& file, int want_w, int want_h,
                 std::vector<std::uint32_t>& out, int& w, int& h) {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj)
        return false;
    bool ok = false;
    if (tj3DecompressHeader(tj, file.data(), file.size()) == 0) {
        const int jw = tj3Get(tj, TJPARAM_JPEGWIDTH), jh = tj3Get(tj, TJPARAM_JPEGHEIGHT);
        if (jw > 0 && jh > 0 && static_cast<std::size_t>(jw) * jh <= k_max_pixels) {
            // The smallest DCT scaling (1/8 … 1) that still covers what is shown: an 8K
            // photo on a 1080p output decodes at a quarter of the work, and the area
            // average that follows sees no fewer pixels than it needs.
            tjscalingfactor best{1, 1};
            int n = 0;
            if (const tjscalingfactor* sf = tj3GetScalingFactors(&n)) {
                for (int i = 0; i < n; ++i) {
                    if (sf[i].num > sf[i].denom)
                        continue;
                    const int sw = TJSCALED(jw, sf[i]), sh = TJSCALED(jh, sf[i]);
                    const int best_w = TJSCALED(jw, best);
                    if (sw >= want_w && sh >= want_h && sw < best_w)
                        best = sf[i];
                }
            }
            if (tj3SetScalingFactor(tj, best) == 0) {
                w = TJSCALED(jw, best);
                h = TJSCALED(jh, best);
                out.resize(static_cast<std::size_t>(w) * h);
                ok = tj3Decompress8(tj, file.data(), file.size(),
                                    reinterpret_cast<unsigned char*>(out.data()), w * 4, TJPF_BGRA) == 0;
            }
        }
    }
    tj3Destroy(tj);
    return ok; // JPEG is opaque: BGRA with alpha 255 is already premultiplied
}
#endif

#if defined(LUMEN_HAS_WEBP)
bool decode_webp(const std::vector<unsigned char>& file, std::vector<std::uint32_t>& out, int& w, int& h) {
    if (!WebPGetInfo(file.data(), file.size(), &w, &h) || w <= 0 || h <= 0 ||
        static_cast<std::size_t>(w) * h > k_max_pixels)
        return false;
    out.resize(static_cast<std::size_t>(w) * h);
    if (!WebPDecodeBGRAInto(file.data(), file.size(), reinterpret_cast<std::uint8_t*>(out.data()),
                            out.size() * 4, w * 4))
        return false;
    premultiply_all(out);
    return true;
}
#endif

#if defined(LUMEN_HAS_RSVG)
bool render_svg(const std::string& path, int w, int h, lx::shell::background_fit fit,
                std::vector<std::uint32_t>& out, int& ow, int& oh) {
    GError* err = nullptr;
    RsvgHandle* svg = rsvg_handle_new_from_file(path.c_str(), &err);
    if (!svg) {
        if (err)
            g_error_free(err);
        return false;
    }
    double iw = 0, ih = 0;
    if (!rsvg_handle_get_intrinsic_size_in_pixels(svg, &iw, &ih) || iw <= 0 || ih <= 0) {
        iw = w; // no intrinsic size (a viewBox only): it scales to whatever it is given
        ih = h;
    }
    fit_size(static_cast<int>(std::lround(iw)), static_cast<int>(std::lround(ih)), w, h, fit, ow, oh);
    if (static_cast<std::size_t>(ow) * oh > k_max_pixels) {
        g_object_unref(svg);
        return false;
    }
    out.assign(static_cast<std::size_t>(ow) * oh, 0u);
    cairo_surface_t* surf = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(out.data()), CAIRO_FORMAT_ARGB32, ow, oh, ow * 4);
    cairo_t* cr = cairo_create(surf);
    const RsvgRectangle viewport{0.0, 0.0, static_cast<double>(ow), static_cast<double>(oh)};
    const bool ok = rsvg_handle_render_document(svg, cr, &viewport, &err) != 0;
    if (err)
        g_error_free(err);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    g_object_unref(svg);
    return ok; // cairo's ARGB32 is premultiplied 0xAARRGGBB already
}
#endif

} // namespace

// ── spec ──────────────────────────────────────────────────────────────────────

std::optional<lx::shell::background_spec> lx::shell::parse_background(std::string_view text) {
    background_spec s{};
    const auto take = [&text]() {
        const auto colon = text.find(':');
        std::string_view head = text.substr(0, colon);
        text = colon == std::string_view::npos ? std::string_view{} : text.substr(colon + 1);
        return head;
    };
    if (text.empty() || text == "lumen" || text == "default")
        return s;
    if (text.front() == '#') {
        s.kind = background_kind::color;
        return parse_hex(text, s.color) ? std::optional{s} : std::nullopt;
    }
    if (text.front() == '/') {
        s.kind = background_kind::image;
        s.path = std::string(text);
        return s;
    }
    const std::string_view kind = take();
    if (kind == "color") {
        s.kind = background_kind::color;
        return parse_hex(text, s.color) ? std::optional{s} : std::nullopt;
    }
    if (kind == "gradient") {
        s.kind = background_kind::gradient;
        if (!parse_hex(take(), s.color) || !parse_hex(take(), s.color2))
            return std::nullopt;
        if (!text.empty()) {
            float deg = 0.f;
            const auto r = std::from_chars(text.data(), text.data() + text.size(), deg);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
                return std::nullopt;
            s.angle_deg = deg;
        }
        return s;
    }
    if (kind == "image") {
        s.kind = background_kind::image;
        // The mode is the last field only when it names one: a path may contain ':'.
        static constexpr std::pair<std::string_view, background_fit> k_modes[] = {
            {":fill", background_fit::fill},       {":fit", background_fit::fit},
            {":center", background_fit::center},   {":stretch", background_fit::stretch},
            {":tile", background_fit::tile}};
        for (const auto& [suffix, fit] : k_modes)
            if (text.size() > suffix.size() && text.ends_with(suffix)) {
                s.fit = fit;
                text.remove_suffix(suffix.size());
                break;
            }
        if (text.empty())
            return std::nullopt;
        s.path = std::string(text);
        return s;
    }
    return std::nullopt;
}

// ── renderers ─────────────────────────────────────────────────────────────────

void lx::shell::render_color(std::uint32_t* px, int w, int h, int stride, std::uint32_t color) {
    for (int y = 0; y < h; ++y)
        std::fill_n(px + static_cast<std::size_t>(y) * stride, w, 0xFF000000u | color);
}

void lx::shell::render_gradient(std::uint32_t* px, int w, int h, int stride, std::uint32_t from,
                                std::uint32_t to, float angle_deg) {
    if (w <= 0 || h <= 0)
        return;
    // Interpolated in linear light, through a table: no pow per pixel.
    constexpr int k_lut = 4096;
    static thread_local std::array<rgbf, k_lut> lut;
    const rgbf a = unpack_rgb(from), b = unpack_rgb(to);
    const rgbf la{srgb_to_linear(a.r), srgb_to_linear(a.g), srgb_to_linear(a.b)};
    const rgbf lb{srgb_to_linear(b.r), srgb_to_linear(b.g), srgb_to_linear(b.b)};
    for (int i = 0; i < k_lut; ++i) {
        const float t = static_cast<float>(i) / (k_lut - 1);
        lut[static_cast<std::size_t>(i)] = {linear_to_srgb(la.r + (lb.r - la.r) * t), linear_to_srgb(la.g + (lb.g - la.g) * t),
                                            linear_to_srgb(la.b + (lb.b - la.b) * t)};
    }
    // t = projection on the direction, normalised so the corners reach 0 and 1.
    const float rad = angle_deg * 3.14159265f / 180.f;
    const float dx = std::cos(rad), dy = std::sin(rad);
    const float span = std::abs(dx) * static_cast<float>(w) + std::abs(dy) * static_cast<float>(h);
    const float cx = static_cast<float>(w) * 0.5f, cy = static_cast<float>(h) * 0.5f;
    const float inv = span > 0.f ? 1.f / span : 0.f;
    for (int y = 0; y < h; ++y) {
        std::uint32_t* row = px + static_cast<std::size_t>(y) * stride;
        const float base = (static_cast<float>(y) + 0.5f - cy) * dy;
        for (int x = 0; x < w; ++x) {
            float t = ((static_cast<float>(x) + 0.5f - cx) * dx + base) * inv + 0.5f;
            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
            const rgbf& c = lut[static_cast<std::size_t>(t * (k_lut - 1) + 0.5f)];
            row[x] = pack_xrgb(c.r, c.g, c.b, dither_ign(x, y));
        }
    }
}

void lx::shell::render_lumen(std::uint32_t* px, int w, int h, int stride) {
    // Lumen's own: a deep navy-to-plum diagonal lit by a soft blue light high on the right
    // and a faint teal one low on the left. Everything is in units of the output's longer
    // edge, so it looks the same at any size and aspect. Per pixel: two rational falloffs
    // (one division each), no transcendental function.
    if (w <= 0 || h <= 0)
        return;
    const float u = 1.f / static_cast<float>(std::max(w, h));
    const rgbf top{0.043f, 0.070f, 0.125f};    // #0B1220
    const rgbf bottom{0.102f, 0.063f, 0.188f}; // #1A1030
    struct light {
        float x, y, r2, cr, cg, cb;
    };
    const float aw = static_cast<float>(w) * u, ah = static_cast<float>(h) * u;
    const light lights[2] = {
        {0.72f * aw, 0.24f * ah, 0.30f, 0.11f, 0.20f, 0.42f}, // the lumen: cool blue
        {0.12f * aw, 1.02f * ah, 0.20f, 0.01f, 0.13f, 0.14f}, // a teal ember
    };
    const float diag = aw + ah;
    for (int y = 0; y < h; ++y) {
        std::uint32_t* row = px + static_cast<std::size_t>(y) * stride;
        const float fy = (static_cast<float>(y) + 0.5f) * u;
        for (int x = 0; x < w; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * u;
            const float t = (fx + fy) / diag;
            float r = top.r + (bottom.r - top.r) * t;
            float g = top.g + (bottom.g - top.g) * t;
            float b = top.b + (bottom.b - top.b) * t;
            for (const light& l : lights) {
                const float ddx = fx - l.x, ddy = fy - l.y;
                const float q = 1.f + (ddx * ddx + ddy * ddy) / l.r2;
                const float f = 1.f / (q * q);
                r += l.cr * f;
                g += l.cg * f;
                b += l.cb * f;
            }
            row[x] = pack_xrgb(r, g, b, dither_ign(x, y));
        }
    }
}

void lx::shell::render_image(std::uint32_t* px, int w, int h, int stride, const std::uint32_t* src,
                             int sw, int sh, background_fit fit, std::uint32_t under) {
    under |= 0xFF000000u;
    if (fit == background_fit::tile) {
        for (int y = 0; y < h; ++y) {
            const std::uint32_t* s = src + static_cast<std::size_t>(y % sh) * sw;
            std::uint32_t* row = px + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < w; ++x)
                row[x] = blend_over(s[x % sw], under);
        }
        return;
    }
    // `src` is already at its shown size (`decode_background` / the caller resampled it);
    // centred, cropped where larger, `under` where smaller.
    const int ox = (w - sw) / 2, oy = (h - sh) / 2;
    for (int y = 0; y < h; ++y) {
        std::uint32_t* row = px + static_cast<std::size_t>(y) * stride;
        const int sy = y - oy;
        if (sy < 0 || sy >= sh) {
            std::fill_n(row, w, under);
            continue;
        }
        const std::uint32_t* s = src + static_cast<std::size_t>(sy) * sw;
        for (int x = 0; x < w; ++x) {
            const int sx = x - ox;
            row[x] = sx < 0 || sx >= sw ? under : blend_over(s[sx], under);
        }
    }
}

bool lx::shell::decode_background(const std::string& path, int w, int h, background_fit fit,
                                  std::vector<std::uint32_t>& out, int& out_w, int& out_h) {
    std::vector<unsigned char> file;
    if (!read_whole_file(path, file) || file.size() < 12)
        return false;
    std::vector<std::uint32_t> raw;
    int iw = 0, ih = 0;
    bool ok = false;
    const unsigned char* m = file.data();
    if (m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G') {
#if defined(LUMEN_HAS_PNG)
        ok = decode_png(file, raw, iw, ih);
#endif
    } else if (m[0] == 0xFF && m[1] == 0xD8) {
#if defined(LUMEN_HAS_JPEG)
        // What the shown size needs, so the DCT can skip the rest.
        int iw0 = 0, ih0 = 0;
        if (tjhandle tj = tj3Init(TJINIT_DECOMPRESS)) {
            if (tj3DecompressHeader(tj, file.data(), file.size()) == 0) {
                iw0 = tj3Get(tj, TJPARAM_JPEGWIDTH);
                ih0 = tj3Get(tj, TJPARAM_JPEGHEIGHT);
            }
            tj3Destroy(tj);
        }
        int want_w = w, want_h = h;
        if (iw0 > 0 && ih0 > 0)
            fit_size(iw0, ih0, w, h, fit, want_w, want_h);
        ok = decode_jpeg(file, want_w, want_h, raw, iw, ih);
#endif
    } else if (std::memcmp(m, "RIFF", 4) == 0 && std::memcmp(m + 8, "WEBP", 4) == 0) {
#if defined(LUMEN_HAS_WEBP)
        ok = decode_webp(file, raw, iw, ih);
#endif
    } else {
#if defined(LUMEN_HAS_RSVG)
        // SVG (or anything else librsvg reads): rendered at the size it is shown.
        return render_svg(path, w, h, fit, out, out_w, out_h);
#endif
    }
    if (!ok || iw <= 0 || ih <= 0)
        return false;
    int sw = iw, sh = ih;
    fit_size(iw, ih, w, h, fit, sw, sh);
    if (sw == iw && sh == ih) {
        out = std::move(raw);
    } else {
        out.assign(static_cast<std::size_t>(sw) * sh, 0u);
        resample_area_bilinear(raw.data(), iw, ih, out.data(), sw, sh);
    }
    out_w = sw;
    out_h = sh;
    return true;
}

lx::shell::background_decoders lx::shell::background_formats() {
    background_decoders d{};
#if defined(LUMEN_HAS_PNG)
    d.png = true;
#endif
#if defined(LUMEN_HAS_JPEG)
    d.jpeg = true;
#endif
#if defined(LUMEN_HAS_WEBP)
    d.webp = true;
#endif
#if defined(LUMEN_HAS_RSVG)
    d.svg = true;
#endif
    return d;
}

bool lx::shell::render_background(const background_spec& spec, std::uint32_t* px, int w, int h,
                                  int stride) {
    switch (spec.kind) {
    case background_kind::lumen:
        render_lumen(px, w, h, stride);
        return true;
    case background_kind::color:
        render_color(px, w, h, stride, spec.color);
        return true;
    case background_kind::gradient:
        render_gradient(px, w, h, stride, spec.color, spec.color2, spec.angle_deg);
        return true;
    case background_kind::image: {
        std::vector<std::uint32_t> img;
        int iw = 0, ih = 0;
        if (decode_background(spec.path, w, h, spec.fit, img, iw, ih)) {
            render_image(px, w, h, stride, img.data(), iw, ih, spec.fit, spec.color);
            return true;
        }
        render_lumen(px, w, h, stride);
        return false;
    }
    }
    return false;
}

// ── the surface ───────────────────────────────────────────────────────────────

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::background::events {
    static background& self(void* data) { return *static_cast<background*>(data); }

    static void configure(void* data, zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t w,
                          uint32_t h) {
        auto& b = self(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        b.width_ = static_cast<int>(w);
        b.height_ = static_cast<int>(h);
        b.configured_ = true;
        b.must_commit_ = true; // the ack takes effect with the next commit
    }
    static void closed(void* data, zwlr_layer_surface_v1*) { self(data).closed_ = true; }

    static void buffer_release(void*, wl_buffer* buffer) {
        // On screen in the compositor's copy: the shell keeps none.
        wl_buffer_destroy(buffer);
    }

#if defined(LUMEN_BG_FRACTIONAL)
    static void preferred_scale(void* data, wp_fractional_scale_v1*, uint32_t scale) {
        auto& b = self(data);
        if (scale != 0 && scale != b.scale120_) {
            b.scale120_ = scale;
            b.must_commit_ = true;
        }
    }
#endif
};

namespace {

const zwlr_layer_surface_v1_listener k_bg_layer_listener = {
    .configure = lx::shell::background::events::configure,
    .closed = lx::shell::background::events::closed,
};
const wl_buffer_listener k_bg_buffer_listener = {
    .release = lx::shell::background::events::buffer_release,
};
#if defined(LUMEN_BG_FRACTIONAL)
const wp_fractional_scale_v1_listener k_bg_fractional_listener = {
    .preferred_scale = lx::shell::background::events::preferred_scale,
};
#endif

} // namespace

lx::shell::background::~background() { destroy(); }

lx::result<void> lx::shell::background::create(background_globals globals, const background_spec* spec,
                                               void* output, int notify_fd) {
    if (!globals.compositor || !globals.shm || !globals.layer_shell || !spec)
        return lx::invalid_argument("background needs wl_compositor, wl_shm and zwlr_layer_shell_v1");
    g_ = globals;
    spec_ = spec;
    notify_fd_ = notify_fd;
    auto* compositor = static_cast<wl_compositor*>(g_.compositor);
    auto* surface = wl_compositor_create_surface(compositor);
    if (!surface)
        return lx::make_error(lx::wayland_err::bind_failed, "wl_compositor.create_surface failed");
    surface_ = surface;
    auto* layer = zwlr_layer_shell_v1_get_layer_surface(static_cast<zwlr_layer_shell_v1*>(g_.layer_shell),
                                                        surface, static_cast<wl_output*>(output),
                                                        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
                                                        "lumen-background");
    layer_ = layer;
    zwlr_layer_surface_v1_add_listener(layer, &k_bg_layer_listener, this);
#if defined(LUMEN_BG_FRACTIONAL)
    if (g_.viewporter) {
        viewport_ = wp_viewporter_get_viewport(static_cast<wp_viewporter*>(g_.viewporter), surface);
        if (g_.fractional) {
            auto* f = wp_fractional_scale_manager_v1_get_fractional_scale(
                static_cast<wp_fractional_scale_manager_v1*>(g_.fractional), surface);
            fractional_ = f;
            wp_fractional_scale_v1_add_listener(f, &k_bg_fractional_listener, this);
        }
    }
#endif
    // The whole output, under the panel too (no exclusive zone of its own, others' ignored).
    zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    // Opaque everywhere (no blending; covered parts are skipped), and no input: the pointer
    // over the bare desktop belongs to no client.
    if (auto* region = wl_compositor_create_region(compositor)) {
        wl_region_add(region, 0, 0, 1 << 30, 1 << 30);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }
    if (auto* none = wl_compositor_create_region(compositor)) {
        wl_surface_set_input_region(surface, none);
        wl_region_destroy(none);
    }
    wl_surface_commit(surface); // no buffer: asks for the configure
    return {};
}

void lx::shell::background::cancel_job() {
    if (!job_)
        return;
    if (job_->thread.joinable())
        job_->thread.join();
    if (job_->data)
        munmap(job_->data, job_->size);
    if (job_->buffer)
        wl_buffer_destroy(static_cast<wl_buffer*>(job_->buffer));
    job_.reset();
}

void lx::shell::background::destroy() {
    cancel_job();
#if defined(LUMEN_BG_FRACTIONAL)
    if (fractional_)
        wp_fractional_scale_v1_destroy(static_cast<wp_fractional_scale_v1*>(fractional_));
    if (viewport_)
        wp_viewport_destroy(static_cast<wp_viewport*>(viewport_));
#endif
    fractional_ = nullptr;
    viewport_ = nullptr;
    if (layer_)
        zwlr_layer_surface_v1_destroy(static_cast<zwlr_layer_surface_v1*>(layer_));
    layer_ = nullptr;
    if (surface_)
        wl_surface_destroy(static_cast<wl_surface*>(surface_));
    surface_ = nullptr;
}

void lx::shell::background::show_color() {
#if defined(LUMEN_BG_SINGLE_PIXEL)
    // One pixel, stretched by the viewport: nothing to render and nothing to hold.
    const auto ch = [](std::uint32_t c, unsigned shift) {
        return static_cast<std::uint32_t>(((c >> shift) & 0xFFu) * 0x01010101u);
    };
    const std::uint32_t c = spec_->color;
    auto* buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        static_cast<wp_single_pixel_buffer_manager_v1*>(g_.single_pixel), ch(c, 16), ch(c, 8),
        ch(c, 0), 0xFFFFFFFFu);
    wl_buffer_add_listener(buffer, &k_bg_buffer_listener, this);
    auto* surface = static_cast<wl_surface*>(surface_);
    wp_viewport_set_destination(static_cast<wp_viewport*>(viewport_), width_, height_);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, 1, 1);
    wl_surface_commit(surface);
    shown_pw_ = width_;
    shown_ph_ = height_;
    shown_ = true;
#endif
}

void lx::shell::background::start_render() {
    // The buffer is made here (Wayland objects stay on this thread); the worker only fills
    // the mapping. A size change mid-render is picked up when that render finishes.
    const bool scaled = viewport_ != nullptr;
    const int pw = scaled ? static_cast<int>(std::lround(width_ * scale120_ / 120.0)) : width_;
    const int ph = scaled ? static_cast<int>(std::lround(height_ * scale120_ / 120.0)) : height_;
    if (pw <= 0 || ph <= 0)
        return;
    const std::size_t size = static_cast<std::size_t>(pw) * ph * 4;
    const int fd = memfd_create("lumen-background", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return;
    }
    (void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return;
    }
    auto* pool = wl_shm_create_pool(static_cast<wl_shm*>(g_.shm), fd, static_cast<int32_t>(size));
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, pw, ph, pw * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool); // the buffer keeps the memory
    close(fd);
    wl_buffer_add_listener(buffer, &k_bg_buffer_listener, this);

    job_ = std::make_unique<job>();
    job_->buffer = buffer;
    job_->data = data;
    job_->size = size;
    job_->pw = pw;
    job_->ph = ph;
    job* j = job_.get();
    const background_spec spec = *spec_;
    const int notify = notify_fd_;
    j->thread = std::thread([j, spec, notify] {
        const auto t0 = std::chrono::steady_clock::now();
        j->ok = render_background(spec, static_cast<std::uint32_t*>(j->data), j->pw, j->ph, j->pw);
        j->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        j->done.store(true, std::memory_order_release);
        const std::uint64_t one = 1;
        if (notify >= 0)
            (void)::write(notify, &one, sizeof(one));
    });
}

void lx::shell::background::finish_render() {
    job& j = *job_;
    j.thread.join();
    if (!j.ok)
        lx::trace::logger::global().logf(lx::trace::level::warn, k_bg_category,
                                         "cannot read background image {}: showing the default",
                                         spec_->path);
    // The compositor maps the memory itself: ours goes now, theirs when it releases.
    munmap(j.data, j.size);
    j.data = nullptr;
    auto* surface = static_cast<wl_surface*>(surface_);
#if defined(LUMEN_BG_FRACTIONAL)
    if (viewport_)
        wp_viewport_set_destination(static_cast<wp_viewport*>(viewport_), width_, height_);
#endif
    wl_surface_attach(surface, static_cast<wl_buffer*>(j.buffer), 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, j.pw, j.ph);
    wl_surface_commit(surface);
    j.buffer = nullptr; // the release listener destroys it
    shown_pw_ = j.pw;
    shown_ph_ = j.ph;
    lx::trace::logger::global().logf(lx::trace::level::info, k_bg_category,
                                     "background {}x{} px, rendered in {:.1f} ms", j.pw, j.ph, j.ms);
    shown_ = true;
    job_.reset();
}

void lx::shell::background::after_dispatch() {
    if (!surface_ || closed_ || !configured_ || width_ <= 0 || height_ <= 0)
        return;
    if (job_) {
        if (!job_->done.load(std::memory_order_acquire))
            return;
        finish_render();
        must_commit_ = false;
    }
    const bool scaled = viewport_ != nullptr;
    const int pw = scaled ? static_cast<int>(std::lround(width_ * scale120_ / 120.0)) : width_;
    const int ph = scaled ? static_cast<int>(std::lround(height_ * scale120_ / 120.0)) : height_;
    const bool solid = spec_->kind == background_kind::color && g_.single_pixel && viewport_;
    const bool stale = solid ? (shown_pw_ != width_ || shown_ph_ != height_) : (shown_pw_ != pw || shown_ph_ != ph);
    if (stale) {
        if (solid)
            show_color();
        else
            start_render();
        must_commit_ = false;
        return;
    }
    if (must_commit_) {
        // A configure with nothing to redraw still needs a commit for its ack.
        wl_surface_commit(static_cast<wl_surface*>(surface_));
        must_commit_ = false;
    }
}

#else

lx::shell::background::~background() = default;
lx::result<void> lx::shell::background::create(background_globals, const background_spec*, void*, int) {
    return lx::not_implemented("lx::shell::background::create");
}
void lx::shell::background::destroy() {}
void lx::shell::background::after_dispatch() {}
void lx::shell::background::start_render() {}
void lx::shell::background::finish_render() {}
void lx::shell::background::show_color() {}
void lx::shell::background::cancel_job() {}

#endif
