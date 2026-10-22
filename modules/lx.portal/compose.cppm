module;

// Screenshot composition (design-app-compat-capture Part E): what xdg-desktop-portal-lumen
// does with the frames it captured over ext-image-copy-capture-v1 before anything is shown
// or saved. Pure and Wayland-free (unit-tested in test_portal_compose).
//
//   - A captured frame arrives in the output's buffer orientation with the transform the
//     compositor applied (ext_image_copy_capture_frame_v1.transform, wl_output.transform
//     values). `upright` turns it back into layout orientation with the compositor's own
//     map (lx.gfx `source_basis_for`, mirrored here and checked against it in the test), so
//     a rotated or flipped output composes like any other.
//   - A region of the layout (logical coordinates) is composed at one scale: the largest
//     scale of the outputs it touches (grim's rule — no output loses pixels), outputs of a
//     smaller scale sampled bilinearly, whole-pixel aligned outputs at their own scale copied
//     row by row. Parts of the region no output covers are opaque black.
//
// Pixels are 0xAARRGGBB words (WL_SHM_FORMAT_XRGB8888 / ARGB8888 little-endian, what the
// compositor offers for shm captures); every composed pixel is opaque.

export module lx.portal:compose;

import std;

import lx.foundation;

export namespace lx::portal {

/// Pixels in rows of `width` words (no padding).
struct image {
    std::vector<std::uint32_t> pixels;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] std::uint32_t at(int x, int y) const {
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)];
    }
    void allocate(int w, int h) {
        width = w > 0 ? w : 0;
        height = h > 0 ? h : 0;
        pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                      0xFF000000u);
    }
};

/// One output as captured, turned upright: `pixels` covers the layout rect `logical`.
struct output_shot {
    std::string name;
    std::string description;
    lx::rect2i logical{};
    image pixels;
};

/// The size of `buffer` (W × H, the buffer orientation) in layout orientation.
[[nodiscard]] lx::size2i upright_size(lx::size2i buffer, lx::buffer_transform transform);

/// Where the layout-oriented pixel (x, y) of a `width` × `height` (layout orientation)
/// image lies in the buffer of transform `transform`: the compositor's content → output
/// pixel map, pixel for pixel.
[[nodiscard]] lx::point2i buffer_pixel_of(int x, int y, lx::buffer_transform transform, int width,
                                          int height);

/// `buffer` (buffer orientation, rows of `stride_px` words) turned upright. Alpha is forced
/// opaque (an XRGB capture's padding byte is undefined).
[[nodiscard]] image upright(const std::uint32_t* buffer, int width, int height, int stride_px,
                            lx::buffer_transform transform);

/// The scale of a shot: its pixel width over its logical width (1 for an empty one).
[[nodiscard]] double shot_scale(const output_shot& shot);

/// The smallest rect holding every shot's logical rect (empty without shots).
[[nodiscard]] lx::rect2i layout_bounds(std::span<const output_shot> shots);

/// grim's rule: the largest scale among the shots that intersect `region` (1 when none).
[[nodiscard]] double compose_scale(std::span<const output_shot> shots, lx::rect2i region);

/// The pixel size of `region` at `scale`.
[[nodiscard]] lx::size2i compose_size(lx::rect2i region, double scale);

/// `region` (logical) of the layout at `scale`: each shot drawn over the part it covers
/// (exact copies where its scale is `scale` and its rect lands on whole pixels, bilinear
/// otherwise), black where none does.
[[nodiscard]] image compose(std::span<const output_shot> shots, lx::rect2i region, double scale);

/// The shot (index) whose logical rect holds the layout point, or -1.
[[nodiscard]] int shot_at(std::span<const output_shot> shots, double x, double y);

/// The captured colour at a layout point (0xFFRRGGBB), from the shot holding it at its own
/// resolution; false when no shot does.
[[nodiscard]] bool colour_at(std::span<const output_shot> shots, double x, double y,
                             std::uint32_t& argb);

/// Each channel scaled by `num` / 256 (the overlay's dimmed backdrop).
void dim(image& img, unsigned num);

} // namespace lx::portal
