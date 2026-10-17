module;

// CRTC and plane allocation (ADR 008 M7): which CRTC, primary plane and cursor plane drive
// each enabled output. Pure — the caller supplies the resources and a test callback (a
// TEST_ONLY commit of the whole candidate state), so it is unit-tested with a fake.
//
// The search is KWin's `checkCrtcAssignment` (a recursive assignment accepted only when the
// full state passes TEST_ONLY) with wlroots' preference for keeping what is routed now
// (`realloc_crtcs`): per connector, its current CRTC first, then the CRTC a previous master
// routed it to (a flicker-free takeover from fbcon), then the remaining possible CRTCs in
// index order. Planes are chosen per CRTC: a primary plane that can reach it (the one bound
// to it now first), and a cursor plane the same way, optional. Bounded: 16 connectors, 32
// CRTCs, at most `k_max_tests` test calls and `k_max_steps` search steps.

export module lx.drm:crtc_alloc;

export namespace lx::drm {

inline constexpr unsigned k_alloc_max_connectors = 16;
inline constexpr unsigned k_alloc_max_crtcs = 32;
inline constexpr unsigned k_alloc_max_planes = 64;

enum class plane_kind : unsigned char { overlay = 0, primary = 1, cursor = 2 };

struct crtc_alloc_connector {
    /// Bit per CRTC index (the OR of the connector's encoders' `possible_crtcs`).
    unsigned possible_crtcs = 0;
    /// The CRTC index this session drives it with now; -1 for none.
    int current_crtc = -1;
    /// The CRTC index it was routed to when the device was opened (a previous master's).
    int routed_crtc = -1;
};

struct crtc_alloc_plane {
    plane_kind kind = plane_kind::overlay;
    unsigned possible_crtcs = 0;
    /// The CRTC index it is bound to now, -1 for none.
    int bound_crtc = -1;
    /// It can scan out the format the compositor uses (XRGB8888 primary, ARGB8888 cursor).
    bool usable = true;
};

struct crtc_alloc_input {
    crtc_alloc_connector connectors[k_alloc_max_connectors]{};
    unsigned connector_count = 0;
    unsigned crtc_count = 0;
    crtc_alloc_plane planes[k_alloc_max_planes]{};
    unsigned plane_count = 0;
};

struct crtc_alloc_result {
    /// Per input connector: CRTC index, plane indices (into `crtc_alloc_input::planes`);
    /// -1 cursor when the CRTC has none.
    int crtc[k_alloc_max_connectors]{};
    int primary_plane[k_alloc_max_connectors]{};
    int cursor_plane[k_alloc_max_connectors]{};
    /// Diagnostics: search steps and test calls made.
    unsigned steps = 0;
    unsigned tests = 0;
};

/// Accepts or rejects a complete candidate (TEST_ONLY of the full state). Null: accept.
using crtc_test_fn = bool (*)(const crtc_alloc_result& candidate, void* user);

inline constexpr unsigned k_alloc_max_tests = 256;
inline constexpr unsigned k_alloc_max_steps = 4096;

/// Finds an assignment of a distinct CRTC and primary plane to every connector of `in`
/// (and a cursor plane where one is free), preferring current routing, accepted by `test`.
/// False when none exists or the bounds ran out; `out` then holds the diagnostics only.
[[nodiscard]] bool assign_crtcs(const crtc_alloc_input& in, crtc_alloc_result& out,
                                crtc_test_fn test, void* user);

} // namespace lx::drm

namespace {

struct search {
    const lx::drm::crtc_alloc_input* in = nullptr;
    lx::drm::crtc_alloc_result* out = nullptr;
    lx::drm::crtc_test_fn test = nullptr;
    void* user = nullptr;
    unsigned used_crtcs = 0;
    bool exhausted = false;
};

/// The plane of `kind` for CRTC `crtc`, not in `used` (bit per plane): with `bound_only`, only
/// the one bound to that CRTC now; otherwise the first in order. -1 for none.
int pick_plane(const lx::drm::crtc_alloc_input& in, lx::drm::plane_kind kind, int crtc,
               unsigned long long used, bool bound_only) {
    for (unsigned p = 0; p < in.plane_count; ++p) {
        const auto& pl = in.planes[p];
        if (pl.kind != kind || !pl.usable || (used >> p) & 1ull ||
            !(pl.possible_crtcs & (1u << static_cast<unsigned>(crtc))))
            continue;
        if (!bound_only || pl.bound_crtc == crtc)
            return static_cast<int>(p);
    }
    return -1;
}

/// Planes of `kind` for the CRTCs chosen so far: first every CRTC keeps the plane bound to it
/// now (no plane moves between CRTCs, which some drivers refuse without a full modeset), then
/// the rest are filled greedily in connector order, which a TEST decides. Returns false when
/// some CRTC got none.
bool assign_kind(search& s, lx::drm::plane_kind kind, int* planes, unsigned long long& used) {
    const auto& in = *s.in;
    const auto& out = *s.out;
    for (unsigned c = 0; c < in.connector_count; ++c)
        planes[c] = -1;
    for (int pass = 0; pass < 2; ++pass) {
        for (unsigned c = 0; c < in.connector_count; ++c) {
            if (planes[c] >= 0)
                continue;
            const int p = pick_plane(in, kind, out.crtc[c], used, pass == 0);
            if (p < 0)
                continue;
            planes[c] = p;
            used |= 1ull << static_cast<unsigned>(p);
        }
    }
    for (unsigned c = 0; c < in.connector_count; ++c)
        if (planes[c] < 0)
            return false;
    return true;
}

/// Planes for the CRTCs chosen so far (every connector has one). False when a CRTC has no
/// primary plane left; a missing cursor plane only means a software cursor there.
bool assign_planes(search& s) {
    auto& out = *s.out;
    unsigned long long used = 0;
    if (!assign_kind(s, lx::drm::plane_kind::primary, out.primary_plane, used))
        return false;
    (void)assign_kind(s, lx::drm::plane_kind::cursor, out.cursor_plane, used);
    return true;
}

bool place(search& s, unsigned c) {
    const auto& in = *s.in;
    auto& out = *s.out;
    if (++out.steps > lx::drm::k_alloc_max_steps) {
        s.exhausted = true;
        return false;
    }
    if (c == in.connector_count) {
        if (!assign_planes(s))
            return false;
        if (!s.test)
            return true;
        if (out.tests >= lx::drm::k_alloc_max_tests) {
            s.exhausted = true;
            return false;
        }
        ++out.tests;
        return s.test(out, s.user);
    }
    const auto& conn = in.connectors[c];
    // Candidate order: current, routed, then by index (each once).
    int order[lx::drm::k_alloc_max_crtcs + 2];
    unsigned n = 0;
    const auto add = [&](int crtc) {
        if (crtc < 0 || static_cast<unsigned>(crtc) >= in.crtc_count ||
            static_cast<unsigned>(crtc) >= lx::drm::k_alloc_max_crtcs)
            return;
        if (!(conn.possible_crtcs & (1u << static_cast<unsigned>(crtc))))
            return;
        for (unsigned i = 0; i < n; ++i)
            if (order[i] == crtc)
                return;
        order[n++] = crtc;
    };
    add(conn.current_crtc);
    add(conn.routed_crtc);
    for (unsigned k = 0; k < in.crtc_count && k < lx::drm::k_alloc_max_crtcs; ++k)
        add(static_cast<int>(k));
    for (unsigned i = 0; i < n && !s.exhausted; ++i) {
        const unsigned bit = 1u << static_cast<unsigned>(order[i]);
        if (s.used_crtcs & bit)
            continue;
        s.used_crtcs |= bit;
        out.crtc[c] = order[i];
        if (place(s, c + 1))
            return true;
        s.used_crtcs &= ~bit;
    }
    return false;
}

} // namespace

bool lx::drm::assign_crtcs(const crtc_alloc_input& in, crtc_alloc_result& out, crtc_test_fn test,
                           void* user) {
    out = {};
    for (unsigned c = 0; c < k_alloc_max_connectors; ++c) {
        out.crtc[c] = -1;
        out.primary_plane[c] = -1;
        out.cursor_plane[c] = -1;
    }
    if (in.connector_count > k_alloc_max_connectors || in.plane_count > k_alloc_max_planes)
        return false;
    if (in.connector_count == 0)
        return true;
    search s{};
    s.in = &in;
    s.out = &out;
    s.test = test;
    s.user = user;
    if (place(s, 0))
        return true;
    const unsigned steps = out.steps;
    const unsigned tests = out.tests;
    out = {};
    for (unsigned c = 0; c < k_alloc_max_connectors; ++c) {
        out.crtc[c] = -1;
        out.primary_plane[c] = -1;
        out.cursor_plane[c] = -1;
    }
    out.steps = steps;
    out.tests = tests;
    return false;
}
