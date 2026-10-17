// Output policy for hotplug (`lx.compositor:output_policy`, ADR 008 §B.2, H3, §B.5): the mode
// an output takes from a probe (following the preferred mode, or keeping an explicit one
// while it is offered), sway's fallback search order when a mode fails its TEST_ONLY, the
// output windows and the pointer fall back to when one goes, and the manager taking a new
// mode or a moved host suggestion.
#include "lumen_test.hpp"
#include "scan_builder.hpp"

import lx.foundation;
import lx.drm;
import lx.compositor;

using lx::compositor::output_manager;
using scan_builder::connector;
using scan_builder::mode;

LUMEN_TEST(policy_choose_mode) {
    lx::drm::connector_scan s{};
    auto& e = connector(s, 44, "Virtual-1", true, {mode(1600, 900), mode(1280, 800), mode(1024, 768)});
    // A new output: preferred.
    const auto* m = lx::compositor::choose_mode(e, nullptr);
    LUMEN_CHECK(m && m->width == 1600);
    lx::compositor::output_state o{};
    o.kms_mode = mode(1280, 800);
    // Following the preferred mode: preferred, whatever the current one.
    o.follow_preferred = true;
    m = lx::compositor::choose_mode(e, &o);
    LUMEN_CHECK(m && m->width == 1600);
    // An explicit mode that is still offered: kept.
    o.follow_preferred = false;
    m = lx::compositor::choose_mode(e, &o);
    LUMEN_CHECK(m && m->width == 1280);
    // Not offered any more: preferred (niri, sway).
    o.kms_mode = mode(3840, 2160);
    m = lx::compositor::choose_mode(e, &o);
    LUMEN_CHECK(m && m->width == 1600);
    // No mode at all.
    lx::drm::connector_probe none{};
    LUMEN_CHECK(lx::compositor::choose_mode(none, nullptr) == nullptr);
}

LUMEN_TEST(policy_fallback_order_is_sways) {
    lx::drm::connector_scan s{};
    auto& e = connector(s, 70, "DP-1", true,
                        {mode(1920, 1080, 60000), mode(2560, 1440, 59951, true), mode(1920, 1080, 144000),
                         mode(1280, 720, 60000), mode(2560, 1440, 59951)});
    lx::drm::kms_mode out[8];
    const lx::drm::kms_mode first = mode(1280, 720, 60000);
    const unsigned n = lx::compositor::mode_candidates(e, &first, out, 8);
    // The chosen one, the preferred one, then by area × refresh, no timing twice.
    LUMEN_CHECK(n == 4);
    LUMEN_CHECK(out[0].width == 1280);
    LUMEN_CHECK(out[1].width == 2560 && out[1].preferred);
    LUMEN_CHECK(out[2].width == 1920 && out[2].refresh_mhz == 144000);
    LUMEN_CHECK(out[3].width == 1920 && out[3].refresh_mhz == 60000);
    // Bounded.
    LUMEN_CHECK(lx::compositor::mode_candidates(e, nullptr, out, 2) == 2);
    LUMEN_CHECK(out[0].preferred);
}

LUMEN_TEST(policy_fallback_output) {
    output_manager m;
    lx::compositor::output_desc d{};
    d.name = "A";
    d.mode = {1920, 1080};
    LUMEN_CHECK(m.add_output(d) == 0);
    d.name = "B";
    d.mode = {1280, 1024};
    LUMEN_CHECK(m.add_output(d) == 1);
    d.name = "C";
    LUMEN_CHECK(m.add_output(d) == 2);
    // The pointer's output unless it is the one going; else the first in layout order.
    LUMEN_CHECK(lx::compositor::fallback_output(m, 1, 2) == 2);
    LUMEN_CHECK(lx::compositor::fallback_output(m, 1, 1) == 0);
    LUMEN_CHECK(lx::compositor::fallback_output(m, 0, -1) == 1);
    // A, C, B left to right: C is first once A goes and the pointer is on A.
    m.set_position(m.nth(2), {1920, 0});
    m.set_position(m.nth(1), {3200, 0});
    LUMEN_CHECK(lx::compositor::fallback_output(m, 0, 0) == 2);
    // Nothing else left.
    output_manager one;
    LUMEN_CHECK(one.add_output(d) == 0);
    LUMEN_CHECK(lx::compositor::fallback_output(one, 0, 0) == -1);
}

LUMEN_TEST(policy_mode_change_relayouts) {
    lx::drm::connector_scan s{};
    connector(s, 44, "Virtual-1", true, {mode(1918, 928)});
    connector(s, 53, "Virtual-2", true, {mode(1024, 768)});
    output_manager m;
    m.configure_from_scan(s);
    LUMEN_CHECK(m.slot_state(1).logical_rect().x == 1918);
    // Virtual-1 shrinks: the automatic output to its right follows.
    m.set_mode(0, mode(1600, 900), true);
    LUMEN_CHECK(m.slot_state(0).mode.width == 1600 && m.slot_state(0).refresh_mhz == 60000);
    LUMEN_CHECK(m.slot_state(1).logical_rect().x == 1600);
    // A host suggestion (VMs) is taken when it does not overlap.
    connector(s, 53, "Virtual-2", true, {mode(1024, 768)});
    s.entries[1].hotplug_mode_update = true;
    s.entries[1].suggested_x = 1700;
    s.entries[1].suggested_y = 0;
    m.update_head(1, s.entries[1]);
    LUMEN_CHECK(m.slot_state(1).logical_rect().x == 1700);
    // Slots: the lowest neither live nor reserved.
    LUMEN_CHECK(m.free_slot(0) == 2);
    LUMEN_CHECK(m.free_slot(1u << 2) == 3);
    // A given slot.
    lx::compositor::output_desc d{};
    d.name = "X";
    d.mode = {640, 480};
    LUMEN_CHECK(m.add_output(d, 5) == 5);
    LUMEN_CHECK(m.add_output(d, 5) == -1);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
