module;

// The policy half of pointer-constraints-unstable-v1 (design-app-compat-capture C.3, PC1):
// Wayland-free, pure, unit-tested (test_pointer_constraints). The wire
// (wire_pointer_constraints.cpp) owns the objects and asks these functions when a
// constraint may hold and where a confined pointer may go.
//
// Activation (Mutter's `should_constraint_be_enabled`, stricter than niri's focus + region):
// the constrained surface has pointer focus, its window has keyboard focus, the pointer is
// inside region ∩ input region ∩ the surface, and nothing else owns the pointer — no
// session lock, no VT pause, no move/resize/drag or popup grab. Super+Escape releases a
// constraint until the pointer leaves the surface or its window loses the keyboard; a
// oneshot constraint is spent at its first deactivation.
//
// Confinement walks the motion segment across the region's rects (wlroots'
// `wlr_region_confine`, weston's idea): into a neighbouring rect where one continues, else
// it stops at the edge and slides along it. Positions are surface-local logical pixels as
// doubles; a rect is half-open, and the farthest a pointer gets towards a right or bottom
// edge is one wl_fixed step (1/256) short of it, so the client is never told a position
// outside the region. UI affinity; no allocation. The bodies are in pointer_constraints.cpp
// (the Clang 23 partition rule, see session_lock.cpp).

import lx.foundation;

export module lx.compositor:pointer_constraints;

import :surface;

export namespace lx::compositor {

/// A surface-local point.
struct constraint_point {
    double x = 0.0;
    double y = 0.0;
};

/// region ∩ input region ∩ the surface's box, as a list of rects (they may overlap).
/// Fixed storage: beyond `k_max` intersections the rest are dropped (`approximated`), which
/// only ever confines more tightly — never lets the pointer out.
struct constraint_region {
    static constexpr unsigned k_max = 32;
    lx::rect2i rects[k_max]{};
    unsigned count = 0;
    bool approximated = false;

    [[nodiscard]] bool empty() const { return count == 0; }
    /// Half-open: [x, x + w) × [y, y + h) of any rect.
    [[nodiscard]] bool contains(double x, double y) const;
};

/// One wl_fixed step: how far inside a right or bottom edge a confined pointer stops.
inline constexpr double k_constraint_edge = 1.0 / 256.0;

/// The effective region of a constraint: `region` (null: the protocol's "null region",
/// i.e. no restriction beyond the input region), intersected with the surface's committed
/// input region and with its box {0, 0, size}. Empty for a surface without content.
[[nodiscard]] constraint_region make_constraint_region(const input_region* region,
                                                       const input_region& input,
                                                       lx::size2i surface_size);

/// The closest point to `p` inside `r` (`p` itself when inside). `p` when `r` is empty.
[[nodiscard]] constraint_point nearest_inside(const constraint_region& r, constraint_point p);

/// Where a pointer confined to `r` that was at `from` ends up when it is moved towards `to`:
/// along the segment as far as the region allows, then sliding along the edge that stopped
/// it. A `from` outside `r` (the region changed under it) starts from `nearest_inside`. The
/// result is inside `r` whenever `r` is not empty.
[[nodiscard]] constraint_point confine_motion(const constraint_region& r, constraint_point from,
                                              constraint_point to);

/// What decides whether one constraint holds. Each flag is about that constraint's surface
/// and the seat right now.
struct constraint_conditions {
    /// The constrained surface has pointer focus.
    bool pointer_focus = false;
    /// The window of the constrained surface (its root) has keyboard focus.
    bool keyboard_focus = false;
    /// The pointer is inside the effective region.
    bool pointer_inside = false;
    /// The session is locked (from the lock request on).
    bool session_locked = false;
    /// The session is paused (VT switched away).
    bool paused = false;
    /// A move/resize, a drag, or a popup grab holds the pointer.
    bool grab = false;
    /// Super+Escape released it and the pointer has not left since.
    bool escaped = false;
    /// A oneshot constraint that was already deactivated, or one whose surface is gone.
    bool defunct = false;
};

/// True when the constraint should be active given `c`; `active` is whether it is now. An
/// active constraint does not need the pointer inside (a lock's region may change under a
/// still pointer; a confined pointer is kept inside by `confine_motion`).
[[nodiscard]] bool constraint_wanted(const constraint_conditions& c, bool active);

/// A lock's cursor position hint is used on unlock only when it lies inside the surface's
/// input region and box (`make_constraint_region(nullptr, input, size)`).
[[nodiscard]] bool constraint_hint_usable(const constraint_region& surface_area,
                                          constraint_point hint);

} // namespace lx::compositor
