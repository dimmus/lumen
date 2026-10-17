// CRTC and plane allocation (`lx.drm:crtc_alloc`, ADR 008 M7): vmwgfx's 1:1 routing, shared
// CRTCs, the preference for current and previously routed CRTCs, backtracking when the test
// (a TEST_ONLY of the whole state, faked here) rejects a candidate, the step and test caps,
// and plane choice (primary bound to the CRTC first, cursor optional, never shared).
#include "lumen_test.hpp"

import lx.drm;

using lx::drm::assign_crtcs;
using lx::drm::crtc_alloc_input;
using lx::drm::crtc_alloc_result;
using lx::drm::plane_kind;

namespace {

/// `n` CRTCs, each with its own primary and cursor plane (vmwgfx: possible_crtcs 1 << i).
void per_crtc_planes(crtc_alloc_input& in, unsigned n) {
    in.crtc_count = n;
    in.plane_count = 0;
    for (unsigned c = 0; c < n; ++c) {
        in.planes[in.plane_count++] = {plane_kind::primary, 1u << c, -1, true};
        in.planes[in.plane_count++] = {plane_kind::cursor, 1u << c, -1, true};
    }
}

} // namespace

LUMEN_TEST(crtc_alloc_vmwgfx_one_to_one) {
    crtc_alloc_input in{};
    per_crtc_planes(in, 8);
    in.connector_count = 3;
    for (unsigned i = 0; i < 3; ++i)
        in.connectors[i].possible_crtcs = 1u << i;
    crtc_alloc_result r{};
    LUMEN_CHECK(assign_crtcs(in, r, nullptr, nullptr));
    for (unsigned i = 0; i < 3; ++i) {
        LUMEN_CHECK(r.crtc[i] == static_cast<int>(i));
        LUMEN_CHECK(in.planes[r.primary_plane[i]].kind == plane_kind::primary);
        LUMEN_CHECK(in.planes[r.primary_plane[i]].possible_crtcs == (1u << i));
        LUMEN_CHECK(r.cursor_plane[i] >= 0 && in.planes[r.cursor_plane[i]].kind == plane_kind::cursor);
    }
}

LUMEN_TEST(crtc_alloc_shared_crtcs_and_too_few) {
    // Two connectors that can both use either of two CRTCs, and one plane each.
    crtc_alloc_input in{};
    per_crtc_planes(in, 2);
    in.connector_count = 2;
    in.connectors[0].possible_crtcs = 0b11;
    in.connectors[1].possible_crtcs = 0b11;
    crtc_alloc_result r{};
    LUMEN_CHECK(assign_crtcs(in, r, nullptr, nullptr));
    LUMEN_CHECK(r.crtc[0] != r.crtc[1]);
    // Three connectors on one CRTC: impossible.
    crtc_alloc_input one{};
    per_crtc_planes(one, 1);
    one.connector_count = 2;
    one.connectors[0].possible_crtcs = 1;
    one.connectors[1].possible_crtcs = 1;
    LUMEN_CHECK(!assign_crtcs(one, r, nullptr, nullptr));
    LUMEN_CHECK(r.crtc[0] == -1);
}

LUMEN_TEST(crtc_alloc_prefers_current_then_routed) {
    crtc_alloc_input in{};
    per_crtc_planes(in, 4);
    in.connector_count = 2;
    in.connectors[0].possible_crtcs = 0b1111;
    in.connectors[1].possible_crtcs = 0b1111;
    // A previous master routed connector 0 to CRTC 2 (fbcon): kept, a flicker-free takeover.
    in.connectors[0].routed_crtc = 2;
    // Connector 1 is driven by CRTC 3 already: kept over everything.
    in.connectors[1].current_crtc = 3;
    in.connectors[1].routed_crtc = 1;
    crtc_alloc_result r{};
    LUMEN_CHECK(assign_crtcs(in, r, nullptr, nullptr));
    LUMEN_CHECK(r.crtc[0] == 2 && r.crtc[1] == 3);
    // Routing to a CRTC the connector cannot use is ignored.
    in.connectors[0].routed_crtc = 7;
    LUMEN_CHECK(assign_crtcs(in, r, nullptr, nullptr));
    LUMEN_CHECK(r.crtc[0] == 0 && r.crtc[1] == 3);
}

namespace {
struct reject_state {
    int forbidden_crtc_for_0 = -1;
    unsigned calls = 0;
};
bool reject_test(const crtc_alloc_result& r, void* user) {
    auto* s = static_cast<reject_state*>(user);
    ++s->calls;
    return r.crtc[0] != s->forbidden_crtc_for_0;
}
bool never(const crtc_alloc_result&, void* user) {
    ++static_cast<reject_state*>(user)->calls;
    return false;
}
} // namespace

LUMEN_TEST(crtc_alloc_backtracks_on_test_rejection) {
    crtc_alloc_input in{};
    per_crtc_planes(in, 3);
    in.connector_count = 2;
    in.connectors[0].possible_crtcs = 0b111;
    in.connectors[1].possible_crtcs = 0b111;
    reject_state s{};
    s.forbidden_crtc_for_0 = 0; // the kernel refuses connector 0 on CRTC 0 (bandwidth, say)
    crtc_alloc_result r{};
    LUMEN_CHECK(assign_crtcs(in, r, &reject_test, &s));
    LUMEN_CHECK(r.crtc[0] != 0 && r.crtc[0] != r.crtc[1]);
    LUMEN_CHECK(s.calls >= 2 && r.tests == s.calls);
}

LUMEN_TEST(crtc_alloc_bounded) {
    // Every candidate refused: the search gives up after the test cap, never loops.
    crtc_alloc_input in{};
    per_crtc_planes(in, 16);
    in.connector_count = 6;
    for (unsigned i = 0; i < 6; ++i)
        in.connectors[i].possible_crtcs = 0xFFFF;
    reject_state s{};
    crtc_alloc_result r{};
    LUMEN_CHECK(!assign_crtcs(in, r, &never, &s));
    LUMEN_CHECK(s.calls <= lx::drm::k_alloc_max_tests);
    LUMEN_CHECK(r.steps <= lx::drm::k_alloc_max_steps + 1);
}

LUMEN_TEST(crtc_alloc_plane_choice) {
    // Two CRTCs, two primary planes that can reach either, one cursor plane for both, and
    // an overlay that must never be taken as a primary.
    crtc_alloc_input in{};
    in.crtc_count = 2;
    in.plane_count = 4;
    in.planes[0] = {plane_kind::overlay, 0b11, -1, true};
    in.planes[1] = {plane_kind::primary, 0b11, 1, true}; // bound to CRTC 1 now
    in.planes[2] = {plane_kind::primary, 0b11, -1, true};
    in.planes[3] = {plane_kind::cursor, 0b11, -1, true};
    in.connector_count = 2;
    in.connectors[0].possible_crtcs = 0b01;
    in.connectors[1].possible_crtcs = 0b10;
    crtc_alloc_result r{};
    LUMEN_CHECK(assign_crtcs(in, r, nullptr, nullptr));
    // CRTC 1 keeps the primary bound to it; CRTC 0 gets the other.
    LUMEN_CHECK(r.primary_plane[1] == 1 && r.primary_plane[0] == 2);
    // One cursor plane: the first CRTC gets it, the other draws a software cursor.
    LUMEN_CHECK(r.cursor_plane[0] == 3 && r.cursor_plane[1] == -1);
    // A primary that cannot scan the compositor's format is not used.
    in.planes[2].usable = false;
    LUMEN_CHECK(!assign_crtcs(in, r, nullptr, nullptr));
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
