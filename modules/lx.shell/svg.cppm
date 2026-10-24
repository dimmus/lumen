module;

import lx.foundation;

export module lx.shell:svg;

import std;

export namespace lx::shell {

/// An SVG document rendered by librsvg, which is loaded (dlopen) the first time one is
/// opened rather than linked: librsvg brings GLib, GIO, cairo, pixman, fontconfig, the
/// XML parser, ICU and more — about half the libraries lumen-shell mapped and a megabyte of
/// their private memory — for a format the panel needs for a few icons, which are cached
/// on disk once rasterised (`icon_theme`), and for an SVG desktop background. A session
/// whose icons are cached never loads it. Without librsvg on the system, `open` fails
/// and callers fall back as they did without SVG support (reported once).
class svg_document {
public:
    svg_document() = default;
    svg_document(const svg_document&) = delete;
    svg_document& operator=(const svg_document&) = delete;
    svg_document(svg_document&& other) noexcept : handle_{std::exchange(other.handle_, nullptr)} {}
    svg_document& operator=(svg_document&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~svg_document() { reset(); }

    /// Parses `path`. False when it is not an SVG librsvg reads, or librsvg is missing.
    [[nodiscard]] bool open(const char* path);
    /// The document's own size in pixels; false when it has none (a viewBox only).
    [[nodiscard]] bool intrinsic_size(double& width, double& height) const;
    /// Renders into `pixels`: premultiplied, native-endian 0xAARRGGBB words (cairo's
    /// ARGB32), `width` x `height`, rows `stride` bytes apart; the document fitted into
    /// `viewport` (x, y, width, height in those pixels). The pixels are drawn over, not
    /// cleared.
    [[nodiscard]] bool render(unsigned char* pixels, int width, int height, int stride,
                              lx::rect2f viewport) const;

private:
    void reset();
    void* handle_ = nullptr; // RsvgHandle*
};

} // namespace lx::shell
