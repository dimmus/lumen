module;

import lx.foundation;

export module lx.compositor:positioner;

export namespace lx::compositor {

/// xdg_positioner state, copied into a popup at get_popup / reposition. Pure data; the
/// enums use the xdg_positioner wire values (anchor and gravity 0..8, adjustment bits).
struct positioner_rules {
    lx::size2i size{};
    bool size_set = false;
    lx::rect2i anchor_rect{};
    bool anchor_rect_set = false;
    unsigned anchor = 0;
    unsigned gravity = 0;
    /// `constraint_adjustment` bits: slide_x 1, slide_y 2, flip_x 4, flip_y 8, resize_x 16,
    /// resize_y 32.
    unsigned adjustment = 0;
    lx::point2i offset{};
    bool reactive = false;
    lx::size2i parent_size{};
    bool parent_size_set = false;
    unsigned parent_configure = 0;
    bool parent_configure_set = false;

    /// xdg_surface.get_popup / xdg_popup.reposition need "a non-zero size set by set_size,
    /// and a non-zero anchor rectangle set by set_anchor_rect" — read literally (both
    /// extents > 0), as Mutter does.
    [[nodiscard]] bool complete() const {
        return size_set && size.width > 0 && size.height > 0 && anchor_rect_set &&
               anchor_rect.width > 0 && anchor_rect.height > 0;
    }
};

/// Placement of a popup: the rect relative to the parent's window geometry, and which
/// adjustments were applied on which axis (bit0 = x, bit1 = y).
struct positioner_result {
    lx::rect2i rect{};
    unsigned flipped = 0;
    unsigned slid = 0;
    unsigned resized = 0;
};

inline constexpr unsigned k_adjust_slide_x = 1;
inline constexpr unsigned k_adjust_slide_y = 2;
inline constexpr unsigned k_adjust_flip_x = 4;
inline constexpr unsigned k_adjust_flip_y = 8;
inline constexpr unsigned k_adjust_resize_x = 16;
inline constexpr unsigned k_adjust_resize_y = 32;

/// The unadjusted rectangle (xdg_positioner: anchor point on the anchor rect, offset, then
/// the box grown in the gravity's direction). Integer halves truncate, as in wlroots.
[[nodiscard]] lx::rect2i positioner_base(const positioner_rules& rules);

/// The anchor (or gravity) value mirrored on one axis: left<->right on x, top<->bottom on y,
/// corners mirrored, an edge on the other axis and `none` unchanged.
[[nodiscard]] unsigned positioner_invert_x(unsigned edge);
[[nodiscard]] unsigned positioner_invert_y(unsigned edge);

/// The base rectangle adjusted to `constraint` (relative to the parent's window geometry,
/// like the result), per axis and in the spec's precedence: 1) flip, 2) slide, 3) resize.
/// A flip uses the original anchor rect and offset with the inverted anchor and gravity,
/// and is kept only when it leaves that axis unconstrained. An empty constraint leaves the
/// base untouched.
[[nodiscard]] positioner_result positioner_place(const positioner_rules& rules,
                                                 lx::rect2i constraint);

} // namespace lx::compositor

namespace {

// Edge sets shared by anchor and gravity (same numbering).
[[nodiscard]] bool has_top(unsigned e) { return e == 1 || e == 5 || e == 7; }
[[nodiscard]] bool has_bottom(unsigned e) { return e == 2 || e == 6 || e == 8; }
[[nodiscard]] bool has_left(unsigned e) { return e == 3 || e == 5 || e == 6; }
[[nodiscard]] bool has_right(unsigned e) { return e == 4 || e == 7 || e == 8; }

[[nodiscard]] bool constrained_x(lx::rect2i b, lx::rect2i c) {
    return b.x < c.x || static_cast<long long>(b.x) + b.width > static_cast<long long>(c.x) + c.width;
}
[[nodiscard]] bool constrained_y(lx::rect2i b, lx::rect2i c) {
    return b.y < c.y ||
           static_cast<long long>(b.y) + b.height > static_cast<long long>(c.y) + c.height;
}

[[nodiscard]] int min_i(long long a, long long b) { return static_cast<int>(a < b ? a : b); }
[[nodiscard]] long long max0(long long v) { return v > 0 ? v : 0; }

} // namespace

unsigned lx::compositor::positioner_invert_x(unsigned e) {
    switch (e) {
    case 3: return 4; // left -> right
    case 4: return 3;
    case 5: return 7; // top_left -> top_right
    case 7: return 5;
    case 6: return 8; // bottom_left -> bottom_right
    case 8: return 6;
    default: return e;
    }
}

unsigned lx::compositor::positioner_invert_y(unsigned e) {
    switch (e) {
    case 1: return 2; // top -> bottom
    case 2: return 1;
    case 5: return 6; // top_left -> bottom_left
    case 6: return 5;
    case 7: return 8; // top_right -> bottom_right
    case 8: return 7;
    default: return e;
    }
}

lx::rect2i lx::compositor::positioner_base(const positioner_rules& r) {
    const lx::rect2i& ar = r.anchor_rect;
    int ax = has_left(r.anchor) ? ar.x : has_right(r.anchor) ? ar.x + ar.width : ar.x + ar.width / 2;
    int ay = has_top(r.anchor) ? ar.y : has_bottom(r.anchor) ? ar.y + ar.height
                                                              : ar.y + ar.height / 2;
    const int w = r.size.width;
    const int h = r.size.height;
    int x = ax + r.offset.x;
    int y = ay + r.offset.y;
    if (has_left(r.gravity))
        x -= w;
    else if (!has_right(r.gravity))
        x -= w / 2;
    if (has_top(r.gravity))
        y -= h;
    else if (!has_bottom(r.gravity))
        y -= h / 2;
    return {x, y, w, h};
}

lx::compositor::positioner_result lx::compositor::positioner_place(const positioner_rules& rules,
                                                                   lx::rect2i c) {
    positioner_result out{};
    lx::rect2i b = positioner_base(rules);
    out.rect = b;
    if (c.width <= 0 || c.height <= 0)
        return out;

    // 1) Flip.
    if (constrained_x(b, c) && (rules.adjustment & k_adjust_flip_x)) {
        positioner_rules f = rules;
        f.anchor = positioner_invert_x(rules.anchor);
        f.gravity = positioner_invert_x(rules.gravity);
        const lx::rect2i fb = positioner_base(f);
        if (!constrained_x(fb, c)) {
            b.x = fb.x;
            out.flipped |= 1;
        }
    }
    if (constrained_y(b, c) && (rules.adjustment & k_adjust_flip_y)) {
        positioner_rules f = rules;
        f.anchor = positioner_invert_y(rules.anchor);
        f.gravity = positioner_invert_y(rules.gravity);
        const lx::rect2i fb = positioner_base(f);
        if (!constrained_y(fb, c)) {
            b.y = fb.y;
            out.flipped |= 2;
        }
    }

    // 2) Slide. Moving towards the constrained edge never crosses the opposite constraint
    // edge, so both of the spec's gravity-ordered phases end in the same place.
    if (constrained_x(b, c) && (rules.adjustment & k_adjust_slide_x)) {
        const long long cr = static_cast<long long>(c.x) + c.width;
        const long long br = static_cast<long long>(b.x) + b.width;
        int d = 0;
        if (b.x < c.x)
            d = min_i(static_cast<long long>(c.x) - b.x, max0(cr - br));
        else if (br > cr)
            d = -min_i(br - cr, max0(static_cast<long long>(b.x) - c.x));
        if (d != 0) {
            b.x += d;
            out.slid |= 1;
        }
    }
    if (constrained_y(b, c) && (rules.adjustment & k_adjust_slide_y)) {
        const long long cb = static_cast<long long>(c.y) + c.height;
        const long long bb = static_cast<long long>(b.y) + b.height;
        int d = 0;
        if (b.y < c.y)
            d = min_i(static_cast<long long>(c.y) - b.y, max0(cb - bb));
        else if (bb > cb)
            d = -min_i(bb - cb, max0(static_cast<long long>(b.y) - c.y));
        if (d != 0) {
            b.y += d;
            out.slid |= 2;
        }
    }

    // 3) Resize: clip to the constraint; with no overlap there is nothing to keep.
    if (constrained_x(b, c) && (rules.adjustment & k_adjust_resize_x)) {
        const long long x0 = b.x > c.x ? b.x : c.x;
        const long long x1 = min_i(static_cast<long long>(b.x) + b.width,
                                   static_cast<long long>(c.x) + c.width);
        if (x1 > x0) {
            b.x = static_cast<int>(x0);
            b.width = static_cast<int>(x1 - x0);
            out.resized |= 1;
        }
    }
    if (constrained_y(b, c) && (rules.adjustment & k_adjust_resize_y)) {
        const long long y0 = b.y > c.y ? b.y : c.y;
        const long long y1 = min_i(static_cast<long long>(b.y) + b.height,
                                   static_cast<long long>(c.y) + c.height);
        if (y1 > y0) {
            b.y = static_cast<int>(y0);
            b.height = static_cast<int>(y1 - y0);
            out.resized |= 2;
        }
    }
    out.rect = b;
    return out;
}
