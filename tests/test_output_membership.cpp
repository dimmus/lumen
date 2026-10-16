// Output membership (wl_surface.enter/leave, preferred buffer scale and transform): which
// outputs a rect is on over one, two and three outputs, edge-touching rects, the primary
// output by area, the preferred scale (ceiling of the largest) and transform; the output
// manager's stable slots, generation counter and headless virtual output.
#include "lumen_test.hpp"

import lx.foundation;
import lx.drm;
import lx.compositor;

using lx::compositor::output_manager;
using lx::compositor::outputs_for_rect;
using lx::compositor::preferred_buffer_scale;
using lx::compositor::preferred_buffer_transform;
using lx::compositor::primary_output_for_rect;

namespace {
/// Three outputs in a row: 1920x1080 at scale 1, 1280x1024 at 1.5, 2560x1440 at 2.
void three(output_manager& m) {
    (void)m.add_virtual("A-1", {0, 0, 1920, 1080}, 60000, 1.f);
    (void)m.add_virtual("B-1", {1920, 0, 1280, 1024}, 75000, 1.5f);
    (void)m.add_virtual("C-1", {3200, 0, 2560, 1440}, 144000, 2.f);
}
} // namespace

LUMEN_TEST(membership_one_output) {
    output_manager m;
    LUMEN_CHECK(m.add_virtual("HEADLESS-1", {0, 0, 1920, 1080}, 60000) == 0);
    LUMEN_CHECK(outputs_for_rect(m, {100, 100, 200, 150}) == 1u);
    LUMEN_CHECK(outputs_for_rect(m, {1800, 1000, 400, 400}) == 1u); // partly off screen
    LUMEN_CHECK(outputs_for_rect(m, {1920, 0, 100, 100}) == 0u);     // touches the right edge
    LUMEN_CHECK(outputs_for_rect(m, {-100, 0, 100, 100}) == 0u);     // touches the left edge
    LUMEN_CHECK(outputs_for_rect(m, {-99, 0, 100, 100}) == 1u);      // one column on it
    LUMEN_CHECK(outputs_for_rect(m, {0, 1080, 10, 10}) == 0u);       // below
    LUMEN_CHECK(outputs_for_rect(m, {10, 10, 0, 10}) == 0u);         // empty
    LUMEN_CHECK(primary_output_for_rect(m, {100, 100, 200, 150}) == 0);
    LUMEN_CHECK(primary_output_for_rect(m, {5000, 0, 10, 10}) == -1);
}

LUMEN_TEST(membership_two_and_three_outputs) {
    output_manager m;
    three(m);
    LUMEN_CHECK(m.count() == 3);
    LUMEN_CHECK(outputs_for_rect(m, {1800, 100, 200, 100}) == 0b011u); // straddles A|B
    LUMEN_CHECK(outputs_for_rect(m, {1900, 0, 1400, 50}) == 0b111u);   // spans all three
    LUMEN_CHECK(outputs_for_rect(m, {1920, 1030, 50, 50}) == 0b000u);  // below B (1024 high)
    LUMEN_CHECK(outputs_for_rect(m, {3199, 0, 2, 2}) == 0b110u);
    // Largest overlap wins: 120 px on A, 80 on B.
    LUMEN_CHECK(primary_output_for_rect(m, {1800, 0, 200, 10}) == 0);
    LUMEN_CHECK(primary_output_for_rect(m, {1860, 0, 200, 10}) == 1);
    // A tie goes to the lower slot.
    LUMEN_CHECK(primary_output_for_rect(m, {1820, 0, 200, 10}) == 0);
}

LUMEN_TEST(membership_preferred_scale_and_transform) {
    output_manager m;
    three(m);
    LUMEN_CHECK(preferred_buffer_scale(m, 0) == 1);       // on no output
    LUMEN_CHECK(preferred_buffer_scale(m, 0b001u) == 1);
    LUMEN_CHECK(preferred_buffer_scale(m, 0b010u) == 2);  // ceil(1.5)
    LUMEN_CHECK(preferred_buffer_scale(m, 0b011u) == 2);  // the max, never blurry
    LUMEN_CHECK(preferred_buffer_scale(m, 0b111u) == 2);
    LUMEN_CHECK(preferred_buffer_scale(m, 1u << 9) == 1); // a dead slot counts as none
    LUMEN_CHECK(preferred_buffer_transform(m, -1) == 0);
    LUMEN_CHECK(preferred_buffer_transform(m, 1) == 0);
    m.set_transform(m.nth(1), 1); // 90°
    LUMEN_CHECK(preferred_buffer_transform(m, 1) == 1);
    LUMEN_CHECK(preferred_buffer_transform(m, 0) == 0);
    LUMEN_CHECK(preferred_buffer_transform(m, 12) == 0);
}

LUMEN_TEST(outputs_keep_their_slot) {
    output_manager m;
    three(m);
    const unsigned g0 = m.generation();
    m.remove_slot(0);
    LUMEN_CHECK(m.count() == 2);
    LUMEN_CHECK(m.generation() != g0);
    // The others did not move: B is still slot 1, C slot 2.
    LUMEN_CHECK(m.slot_live(1) && m.slot_live(2) && !m.slot_live(0));
    LUMEN_CHECK(outputs_for_rect(m, {1920, 0, 10, 10}) == 0b010u);
    LUMEN_CHECK(m.nth(0) == m.slot_state(1).id);
    // A new output takes the free slot.
    LUMEN_CHECK(m.add_virtual("D-1", {0, 0, 800, 600}, 60000) == 0);
    LUMEN_CHECK(m.live_mask() == 0b111u);
    // An unchanged scale moves nothing; a changed one does.
    const unsigned g1 = m.generation();
    m.set_scale(m.nth(1), 1.5f);
    LUMEN_CHECK(m.generation() == g1);
    m.set_scale(m.nth(1), 2.f);
    LUMEN_CHECK(m.generation() != g1);
}

LUMEN_TEST(headless_output_without_connectors) {
    output_manager m;
    lx::drm::kms_device none; // never opened: no connectors
    m.refresh_from_drm(none);
    LUMEN_CHECK(m.count() == 1);
    const auto o = m.state(m.nth(0));
    LUMEN_CHECK(o.virtual_output);
    LUMEN_CHECK(o.geometry.width == 1920 && o.geometry.height == 1080);
    LUMEN_CHECK(o.refresh_mhz == 60000);
    LUMEN_CHECK(o.name[0] == 'H' && std::strcmp(o.name, "HEADLESS-1") == 0);
    // The hotplug poll refreshes every tick: nothing changed, nothing to tell clients.
    const unsigned g = m.generation();
    m.refresh_from_drm(none);
    m.refresh_from_drm(none);
    LUMEN_CHECK(m.generation() == g);
    LUMEN_CHECK(m.count() == 1 && m.slot_of(m.nth(0)) == 0);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
