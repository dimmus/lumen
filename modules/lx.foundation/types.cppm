module;

export module lx.foundation:types;

export namespace lx {

struct point2i {
    int x = 0;
    int y = 0;
    constexpr point2i() = default;
    constexpr point2i(int x_, int y_) : x{x_}, y{y_} {}
    constexpr point2i operator+(point2i o) const { return {x + o.x, y + o.y}; }
};

struct size2i {
    int width = 0;
    int height = 0;
    constexpr size2i() = default;
    constexpr size2i(int w, int h) : width{w}, height{h} {}
    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
};

struct rect2i {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    [[nodiscard]] constexpr point2i origin() const { return {x, y}; }
    [[nodiscard]] constexpr size2i size() const { return {width, height}; }
    [[nodiscard]] constexpr bool contains(point2i p) const;
};

struct inset {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;
};

/// CIE xy chromaticity primaries for a color space.
struct color_primaries {
    float red_x = 0.64f;
    float red_y = 0.33f;
    float green_x = 0.30f;
    float green_y = 0.60f;
    float blue_x = 0.15f;
    float blue_y = 0.06f;
    float white_x = 0.3127f;
    float white_y = 0.3290f;
};

enum class color_space : unsigned char {
    srgb,
    display_p3,
    bt2020,
    scrgb,
};

enum class transfer_function : unsigned char {
    linear,
    srgb,
    gamma22,
    pq,
    hlg,
};

/// 2×3 affine transform (column-major): [m0 m2 tx; m1 m3 ty].
struct transform2d {
    float m0 = 1.f;
    float m1 = 0.f;
    float m2 = 0.f;
    float m3 = 1.f;
    float tx = 0.f;
    float ty = 0.f;

    [[nodiscard]] static constexpr transform2d identity() { return {}; }
    [[nodiscard]] static constexpr transform2d translate(int x, int y) {
        return {1.f, 0.f, 0.f, 1.f, static_cast<float>(x), static_cast<float>(y)};
    }
    [[nodiscard]] static constexpr transform2d scale(float sx, float sy) {
        return {sx, 0.f, 0.f, sy, 0.f, 0.f};
    }
};

/// wl_output.transform values (0–7).
enum class buffer_transform : unsigned char {
    normal = 0,
    rotate_90 = 1,
    rotate_180 = 2,
    rotate_270 = 3,
    flipped = 4,
    flipped_90 = 5,
    flipped_180 = 6,
    flipped_270 = 7,
};

enum class blend_mode : unsigned char {
    opaque,
    premultiplied,
    additive,
};

/// Packed draw sort key: high 8 bits = layer, low 24 = z-order within layer.
struct draw_sort_key {
    unsigned value = 0;

    [[nodiscard]] static constexpr draw_sort_key make(unsigned layer, unsigned z_order) {
        return {(layer << 24u) | (z_order & 0x00FFFFFFu)};
    }
    [[nodiscard]] constexpr unsigned layer() const { return value >> 24u; }
    [[nodiscard]] constexpr unsigned z_order() const { return value & 0x00FFFFFFu; }
};

struct color {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
    color_space space = color_space::srgb;
    transfer_function encoding = transfer_function::srgb;

    static constexpr color rgb(float r_, float g_, float b_, float a_ = 1.f) {
        return {r_, g_, b_, a_, color_space::srgb, transfer_function::srgb};
    }

    [[nodiscard]] static constexpr color linear(float r_, float g_, float b_, float a_ = 1.f,
                                                color_space cs = color_space::srgb) {
        return {r_, g_, b_, a_, cs, transfer_function::linear};
    }

    [[nodiscard]] static constexpr color encoded(float r_, float g_, float b_, float a_ = 1.f,
                                                 color_space cs = color_space::srgb,
                                                 transfer_function tf = transfer_function::srgb) {
        return {r_, g_, b_, a_, cs, tf};
    }
};

[[nodiscard]] constexpr color_primaries primaries_for(color_space cs) {
    switch (cs) {
    case color_space::display_p3:
        return {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f};
    case color_space::bt2020:
        return {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
    case color_space::scrgb:
        return {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
    case color_space::srgb:
    default:
        return {};
    }
}

using fourcc = unsigned;

enum class pixel_format : fourcc {
    argb8888 = 0x34325241,
    xrgb8888 = 0x34325258,
    rgba8888 = 0x41424752,
    argb2101010 = 0x30335241,
    xrgb2101010 = 0x30335258,
    abgr16f = 0x48324741,
};

} // namespace lx


constexpr bool lx::rect2i::contains(lx::point2i p) const {
    return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height;
}
