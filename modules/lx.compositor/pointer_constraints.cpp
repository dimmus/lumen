module;

// Bodies of `:pointer_constraints`. See pointer_constraints.cppm.

#include <cmath>

module lx.compositor;

import lx.foundation;
import :surface;
import :pointer_constraints;

namespace lx::compositor {

namespace {

[[nodiscard]] long long right_of(lx::rect2i r) { return static_cast<long long>(r.x) + r.width; }
[[nodiscard]] long long bottom_of(lx::rect2i r) { return static_cast<long long>(r.y) + r.height; }

/// a ∩ b, or an empty rect.
[[nodiscard]] lx::rect2i intersect(lx::rect2i a, lx::rect2i b) {
    const long long x0 = a.x > b.x ? a.x : b.x;
    const long long y0 = a.y > b.y ? a.y : b.y;
    const long long x1 = right_of(a) < right_of(b) ? right_of(a) : right_of(b);
    const long long y1 = bottom_of(a) < bottom_of(b) ? bottom_of(a) : bottom_of(b);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

[[nodiscard]] bool rect_contains(lx::rect2i r, double x, double y) {
    return r.width > 0 && r.height > 0 && x >= r.x && y >= r.y &&
           x < static_cast<double>(right_of(r)) && y < static_cast<double>(bottom_of(r));
}

/// `p` clamped into `r` (half-open: one wl_fixed step short of the far edges).
[[nodiscard]] constraint_point clamp_into(lx::rect2i r, constraint_point p) {
    const double x1 = static_cast<double>(right_of(r)) - k_constraint_edge;
    const double y1 = static_cast<double>(bottom_of(r)) - k_constraint_edge;
    constraint_point q = p;
    if (q.x < r.x)
        q.x = r.x;
    if (q.x > x1)
        q.x = x1;
    if (q.y < r.y)
        q.y = r.y;
    if (q.y > y1)
        q.y = y1;
    return q;
}

/// The rect of `r` containing `p` that reaches farthest in the direction (dx, dy), or -1.
[[nodiscard]] int box_at(const constraint_region& r, constraint_point p, double dx, double dy) {
    int best = -1;
    double reach = -1.0;
    for (unsigned i = 0; i < r.count; ++i) {
        const lx::rect2i& b = r.rects[i];
        if (!rect_contains(b, p.x, p.y))
            continue;
        // How far the segment could run inside this rect before leaving it.
        double tx = 1e300;
        double ty = 1e300;
        if (dx > 0)
            tx = (static_cast<double>(right_of(b)) - p.x) / dx;
        else if (dx < 0)
            tx = (b.x - p.x) / dx;
        if (dy > 0)
            ty = (static_cast<double>(bottom_of(b)) - p.y) / dy;
        else if (dy < 0)
            ty = (b.y - p.y) / dy;
        const double t = tx < ty ? tx : ty;
        if (t > reach) {
            reach = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

} // namespace

bool constraint_region::contains(double x, double y) const {
    for (unsigned i = 0; i < count; ++i)
        if (rect_contains(rects[i], x, y))
            return true;
    return false;
}

constraint_region make_constraint_region(const input_region* region, const input_region& input,
                                         lx::size2i surface_size) {
    constraint_region out{};
    if (surface_size.width <= 0 || surface_size.height <= 0)
        return out;
    const lx::rect2i box{0, 0, surface_size.width, surface_size.height};
    // Each side as a list of rects; "infinite" is the surface's box.
    const lx::rect2i* a = &box;
    unsigned na = 1;
    if (region && !region->infinite) {
        a = region->rects;
        na = region->count;
    }
    const lx::rect2i* b = &box;
    unsigned nb = 1;
    if (!input.infinite) {
        b = input.rects;
        nb = input.count;
    }
    for (unsigned i = 0; i < na; ++i) {
        const lx::rect2i ra = intersect(a[i], box);
        if (ra.width <= 0)
            continue;
        for (unsigned j = 0; j < nb; ++j) {
            const lx::rect2i r = intersect(ra, b[j]);
            if (r.width <= 0)
                continue;
            if (out.count == constraint_region::k_max) {
                out.approximated = true;
                return out;
            }
            out.rects[out.count++] = r;
        }
    }
    return out;
}

constraint_point nearest_inside(const constraint_region& r, constraint_point p) {
    if (r.contains(p.x, p.y))
        return p;
    constraint_point best = p;
    double best_d = -1.0;
    for (unsigned i = 0; i < r.count; ++i) {
        const constraint_point q = clamp_into(r.rects[i], p);
        const double d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = q;
        }
    }
    return best;
}

constraint_point confine_motion(const constraint_region& r, constraint_point from,
                                constraint_point to) {
    if (r.empty())
        return from;
    constraint_point p = r.contains(from.x, from.y) ? from : nearest_inside(r, from);
    constraint_point target = to;
    // Every iteration either ends, enters another rect further along the segment, or stops
    // one axis (at most twice in a row): bounded well below this.
    for (unsigned iter = 0; iter < 64; ++iter) {
        const double dx = target.x - p.x;
        const double dy = target.y - p.y;
        if (dx == 0.0 && dy == 0.0)
            return p;
        const int bi = box_at(r, p, dx, dy);
        if (bi < 0)
            return nearest_inside(r, p);
        const lx::rect2i& b = r.rects[bi];
        if (rect_contains(b, target.x, target.y))
            return target;
        // The parametric exit from b along (dx, dy).
        double tx = 1e300;
        double ty = 1e300;
        if (dx > 0)
            tx = (static_cast<double>(right_of(b)) - p.x) / dx;
        else if (dx < 0)
            tx = (b.x - p.x) / dx;
        if (dy > 0)
            ty = (static_cast<double>(bottom_of(b)) - p.y) / dy;
        else if (dy < 0)
            ty = (b.y - p.y) / dy;
        const bool exit_x = tx <= ty;
        const bool exit_y = ty <= tx;
        const double t = exit_x ? tx : ty;
        constraint_point e{p.x + dx * t, p.y + dy * t};
        // Just past the edge it crosses: the far edge itself (half-open rects), or one wl_fixed
        // step before the near edge.
        constraint_point q = e;
        if (exit_x)
            q.x = dx > 0 ? static_cast<double>(right_of(b)) : b.x - k_constraint_edge;
        if (exit_y)
            q.y = dy > 0 ? static_cast<double>(bottom_of(b)) : b.y - k_constraint_edge;
        if (r.contains(q.x, q.y)) {
            p = q;
            continue;
        }
        // At a corner, the motion may still go on along one axis into a neighbour.
        if (exit_x && exit_y) {
            const constraint_point qx{q.x, e.y};
            const constraint_point qy{e.x, q.y};
            const bool via_x = r.contains(qx.x, qx.y);
            const bool via_y = r.contains(qy.x, qy.y);
            if (via_x || via_y) {
                const bool take_x = via_x && (!via_y || std::fabs(dx) >= std::fabs(dy));
                p = take_x ? qx : qy;
                // The other axis is stopped by the corner it ran into.
                if (take_x)
                    target.y = p.y;
                else
                    target.x = p.x;
                continue;
            }
        }
        // Blocked: stop at the edge and slide along it with what is left of the motion.
        p = clamp_into(b, e);
        if (exit_x)
            target.x = p.x;
        if (exit_y)
            target.y = p.y;
    }
    return r.contains(p.x, p.y) ? p : nearest_inside(r, p);
}

bool constraint_wanted(const constraint_conditions& c, bool active) {
    if (c.defunct || c.session_locked || c.paused || c.grab || c.escaped)
        return false;
    if (!c.pointer_focus || !c.keyboard_focus)
        return false;
    return active || c.pointer_inside;
}

bool constraint_hint_usable(const constraint_region& surface_area, constraint_point hint) {
    return surface_area.contains(hint.x, hint.y);
}

} // namespace lx::compositor
