module;

// Bodies of `:png`.

#include <unistd.h>

#if defined(LUMEN_PORTAL_PNG)
#include <png.h>
#endif
#include <setjmp.h>

module lx.portal;

import std;

import lx.foundation;
import :compose;
import :png;

namespace lx::portal {

#if defined(LUMEN_PORTAL_PNG)

namespace {

struct fd_sink {
    int fd = -1;
    bool failed = false;
};

void write_fn(png_structp png, png_bytep data, png_size_t len) {
    auto* sink = static_cast<fd_sink*>(png_get_io_ptr(png));
    while (len > 0 && !sink->failed) {
        const ssize_t n = ::write(sink->fd, data, len);
        if (n <= 0) {
            sink->failed = true;
            break;
        }
        data += n;
        len -= static_cast<png_size_t>(n);
    }
}

void flush_fn(png_structp) {}

} // namespace

lx::result<void> write_png(int fd, const image& img, int level) {
    if (fd < 0 || img.empty())
        return lx::make_error(lx::error_domain::invalid_argument, 0, "write_png: no file or no image");
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png)
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::broken_pipe),
                              "png_create_write_struct failed");
    png_infop info = png_create_info_struct(png);
    fd_sink sink{fd, false};
    std::vector<png_bytep> rows;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, info ? &info : nullptr);
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::broken_pipe),
                              "PNG encoding failed");
    }
    png_set_write_fn(png, &sink, write_fn, flush_fn);
    png_set_IHDR(png, info, static_cast<png_uint_32>(img.width), static_cast<png_uint_32>(img.height),
                 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, level < 0 ? 0 : (level > 9 ? 9 : level));
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
    png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    png_write_info(png, info);
    // The words are B, G, R, X in memory: libpng swaps to RGB and strips the filler.
    png_set_bgr(png);
    png_set_filler(png, 0, PNG_FILLER_AFTER);
    rows.resize(static_cast<std::size_t>(img.height));
    for (int y = 0; y < img.height; ++y)
        rows[static_cast<std::size_t>(y)] = reinterpret_cast<png_bytep>(
            const_cast<std::uint32_t*>(img.pixels.data() + static_cast<std::size_t>(y) * img.width));
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    if (sink.failed)
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::broken_pipe),
                              "writing the PNG file failed");
    return {};
}

lx::result<image> read_png(const char* path) {
    png_image pi{};
    pi.version = PNG_IMAGE_VERSION;
    if (!path || !png_image_begin_read_from_file(&pi, path))
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::not_found),
                              "PNG not readable");
    pi.format = PNG_FORMAT_BGRA;
    image img;
    img.allocate(static_cast<int>(pi.width), static_cast<int>(pi.height));
    if (!png_image_finish_read(&pi, nullptr, img.pixels.data(), 0, nullptr)) {
        png_image_free(&pi);
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::broken_pipe),
                              "PNG decoding failed");
    }
    return img;
}

#else

lx::result<void> write_png(int, const image&, int) {
    return lx::not_implemented("lx::portal::write_png");
}

lx::result<image> read_png(const char*) { return lx::not_implemented("lx::portal::read_png"); }

#endif

} // namespace lx::portal
