export module lx.gfx:damage;

import lx.foundation;

export namespace lx::gfx {

/// Rect capacity of a damage region. Equal to `drm::kms_damage_region`'s, so a region maps
/// one-to-one onto FB_DAMAGE_CLIPS without a second merge pass.
inline constexpr unsigned k_damage_rects = 16;

/// A set of damaged rectangles with fixed capacity and no allocation.
///
/// A single bounding box is the wrong shape for window motion: moving a window from one
/// corner of the screen to the other damages two small rects and a bbox spanning the whole
/// display. A full region (pixman) is the right shape but allocates. This keeps up to 16
/// rects and merges two only when the merged rect wastes at most 25 % over their union,
/// so a 3 px drag stays one rect with almost no overdraw while a long move stays two.
///
/// Rects may overlap. Every consumer (composite clear+draw, KMS clips) is idempotent over
/// an overlap, so overlap costs a little overdraw and never correctness.
struct damage_region {
    lx::rect2i rects[k_damage_rects]{};
    unsigned count = 0;
    /// The whole target is damaged; `rects` is ignored.
    bool full = false;

    void clear() {
        count = 0;
        full = false;
    }
    void set_full() {
        count = 0;
        full = true;
    }
    [[nodiscard]] bool empty() const { return !full && count == 0; }

    /// Adds one rect, merging per the 25 % waste rule; on overflow merges the cheapest pair.
    void add(lx::rect2i r);
    /// Adds every rect of `other`; a full `other` makes this full. A distinct name, not an
    /// `add` overload: `add({x, y, w, h})` would otherwise be ambiguous, since a braced
    /// list can aggregate-initialise either parameter.
    void add_region(const damage_region& other);
    /// Intersects each rect with `bounds` and drops the empty ones. Full becomes `{bounds}`.
    void clip(lx::rect2i bounds);
    /// Bounding box of all rects. Empty for an empty region and for a full one — the
    /// caller knows the target's extent, this does not.
    [[nodiscard]] lx::rect2i bounds() const;
    /// Sum of rect areas (overlaps counted twice).
    [[nodiscard]] unsigned long long area() const;
};

} // namespace lx::gfx

namespace lx::gfx::detail {

[[nodiscard]] inline bool damage_rect_empty(lx::rect2i r) { return r.width <= 0 || r.height <= 0; }

[[nodiscard]] inline long long damage_area(lx::rect2i r) {
    return damage_rect_empty(r) ? 0 : static_cast<long long>(r.width) * r.height;
}

[[nodiscard]] inline bool damage_contains(lx::rect2i outer, lx::rect2i inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           static_cast<long long>(inner.x) + inner.width <=
               static_cast<long long>(outer.x) + outer.width &&
           static_cast<long long>(inner.y) + inner.height <=
               static_cast<long long>(outer.y) + outer.height;
}

[[nodiscard]] inline lx::rect2i damage_bbox(lx::rect2i a, lx::rect2i b) {
    const long long x0 = a.x < b.x ? a.x : b.x;
    const long long y0 = a.y < b.y ? a.y : b.y;
    const long long ax1 = static_cast<long long>(a.x) + a.width;
    const long long bx1 = static_cast<long long>(b.x) + b.width;
    const long long ay1 = static_cast<long long>(a.y) + a.height;
    const long long by1 = static_cast<long long>(b.y) + b.height;
    const long long x1 = ax1 > bx1 ? ax1 : bx1;
    const long long y1 = ay1 > by1 ? ay1 : by1;
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

[[nodiscard]] inline lx::rect2i damage_intersect(lx::rect2i a, lx::rect2i b) {
    const long long x0 = a.x > b.x ? a.x : b.x;
    const long long y0 = a.y > b.y ? a.y : b.y;
    const long long ax1 = static_cast<long long>(a.x) + a.width;
    const long long bx1 = static_cast<long long>(b.x) + b.width;
    const long long ay1 = static_cast<long long>(a.y) + a.height;
    const long long by1 = static_cast<long long>(b.y) + b.height;
    const long long x1 = ax1 < bx1 ? ax1 : bx1;
    const long long y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

/// Exact union area of two rects.
[[nodiscard]] inline long long damage_union_area(lx::rect2i a, lx::rect2i b) {
    return damage_area(a) + damage_area(b) - damage_area(damage_intersect(a, b));
}

} // namespace lx::gfx::detail

void lx::gfx::damage_region::add(lx::rect2i r) {
    using namespace lx::gfx::detail;
    if (full || damage_rect_empty(r))
        return;

    // Absorb until stable: a merged rect may now swallow, or be cheap to merge with,
    // rects it did not touch before. Each pass removes at least one rect, so this ends.
    for (;;) {
        for (unsigned i = 0; i < count; ++i) {
            if (damage_contains(rects[i], r))
                return;
        }
        unsigned kept = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!damage_contains(r, rects[i]))
                rects[kept++] = rects[i];
        }
        count = kept;

        bool merged = false;
        for (unsigned i = 0; i < count; ++i) {
            const lx::rect2i b = damage_bbox(rects[i], r);
            // bbox <= 1.25 x union, in integers.
            if (damage_area(b) * 4 <= damage_union_area(rects[i], r) * 5) {
                rects[i] = rects[--count];
                r = b;
                merged = true;
                break;
            }
        }
        if (!merged)
            break;
    }

    if (count < k_damage_rects) {
        rects[count++] = r;
        return;
    }

    // Full: merge the pair, among the stored rects and `r`, whose bbox wastes the least.
    // 17 candidates, 136 pairs — negligible, and it only runs on overflow.
    const auto at = [&](unsigned i) { return i < count ? rects[i] : r; };
    unsigned best_a = 0;
    unsigned best_b = 1;
    long long best_waste = -1;
    for (unsigned a = 0; a <= count; ++a) {
        for (unsigned b = a + 1; b <= count; ++b) {
            const lx::rect2i ra = at(a);
            const lx::rect2i rb = at(b);
            const long long waste = damage_area(damage_bbox(ra, rb)) - damage_union_area(ra, rb);
            if (best_waste < 0 || waste < best_waste) {
                best_waste = waste;
                best_a = a;
                best_b = b;
            }
        }
    }
    const lx::rect2i merged = damage_bbox(at(best_a), at(best_b));
    // `r` sits at index `count`; best_a < best_b, so only best_b can be it. Swap-remove the
    // stored members, higher index first so the lower index stays valid.
    const bool r_in_pair = best_b == count;
    if (!r_in_pair)
        rects[best_b] = rects[--count];
    rects[best_a] = rects[--count];
    if (!r_in_pair)
        add(r); // still needs a place; at most 15 stored, so it fits
    add(merged);
}

void lx::gfx::damage_region::add_region(const damage_region& other) {
    if (full)
        return;
    if (other.full) {
        set_full();
        return;
    }
    for (unsigned i = 0; i < other.count; ++i)
        add(other.rects[i]);
}

void lx::gfx::damage_region::clip(lx::rect2i bounds) {
    using namespace lx::gfx::detail;
    if (full) {
        full = false;
        count = 0;
        if (!damage_rect_empty(bounds))
            rects[count++] = bounds;
        return;
    }
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        const lx::rect2i c = damage_intersect(rects[i], bounds);
        if (!damage_rect_empty(c))
            rects[kept++] = c;
    }
    count = kept;
}

lx::rect2i lx::gfx::damage_region::bounds() const {
    if (full || count == 0)
        return {};
    lx::rect2i b = rects[0];
    for (unsigned i = 1; i < count; ++i)
        b = detail::damage_bbox(b, rects[i]);
    return b;
}

unsigned long long lx::gfx::damage_region::area() const {
    unsigned long long total = 0;
    for (unsigned i = 0; i < count; ++i)
        total += static_cast<unsigned long long>(detail::damage_area(rects[i]));
    return total;
}
