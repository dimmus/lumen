// The output layout (`lx.compositor:output`, ADR 008 M1, M2, M8, M9): the automatic
// left-to-right layout (scale-aware, gaps close, explicit positions stay, a VM host's
// suggested positions unless they overlap), the pointer's union clamp and the absolute
// device's bounding box, the output under a point, the pace output (wlroots' 10 % rule),
// sway's default scale, monitor identifiers and the start-up scan.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.drm;
import lx.compositor;

using lx::compositor::output_desc;
using lx::compositor::output_manager;

namespace {

[[nodiscard]] bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

int add(output_manager& m, const char* name, int w, int h, double scale = 1.0) {
    output_desc d{};
    d.name = name;
    d.mode = {w, h};
    d.scale = scale;
    return m.add_output(d);
}

} // namespace

LUMEN_TEST(layout_auto_left_to_right_scale_aware) {
    output_manager m;
    LUMEN_CHECK(add(m, "DP-1", 1920, 1080) == 0);
    LUMEN_CHECK(add(m, "DP-2", 2560, 1440, 2.0) == 1); // 1280x720 logical
    LUMEN_CHECK(add(m, "HDMI-A-1", 1280, 1024) == 2);
    LUMEN_CHECK(same(m.slot_state(0).logical_rect(), {0, 0, 1920, 1080}));
    LUMEN_CHECK(same(m.slot_state(1).logical_rect(), {1920, 0, 1280, 720}));
    LUMEN_CHECK(same(m.slot_state(2).logical_rect(), {3200, 0, 1280, 1024}));
    // The middle one goes: the gap closes.
    const unsigned g = m.layout_generation();
    m.remove_slot(1);
    LUMEN_CHECK(m.layout_generation() != g);
    LUMEN_CHECK(same(m.slot_state(2).logical_rect(), {1920, 0, 1280, 1024}));
    // A scale change moves the outputs to its right.
    m.set_scale(m.slot_state(0).id, 1.5); // 1280x720 logical
    LUMEN_CHECK(same(m.slot_state(2).logical_rect(), {1280, 0, 1280, 1024}));
}

LUMEN_TEST(layout_explicit_positions_stay) {
    output_manager m;
    (void)add(m, "A", 1000, 800);
    (void)add(m, "B", 800, 600);
    m.set_position(m.slot_state(0).id, {500, 300}); // explicit: leaves the automatic layout
    LUMEN_CHECK(!m.slot_state(0).auto_position);
    // B goes right of the rightmost placed output.
    LUMEN_CHECK(same(m.slot_state(1).logical_rect(), {1500, 0, 800, 600}));
    (void)add(m, "C", 640, 480);
    LUMEN_CHECK(same(m.slot_state(2).logical_rect(), {2300, 0, 640, 480}));
    m.remove_slot(1);
    LUMEN_CHECK(same(m.slot_state(0).logical_rect(), {500, 300, 1000, 800}));
    LUMEN_CHECK(same(m.slot_state(2).logical_rect(), {1500, 0, 640, 480}));
}

LUMEN_TEST(layout_suggested_positions_of_a_vm_host) {
    output_manager m;
    output_desc a{};
    a.name = "Virtual-1";
    a.mode = {1918, 928};
    a.hotplug_mode_update = true;
    a.suggested_x = 0;
    a.suggested_y = 0;
    output_desc b = a;
    b.name = "Virtual-2";
    b.mode = {1024, 768};
    b.suggested_x = 1918;
    b.suggested_y = 160; // the host put it lower
    (void)m.add_output(a);
    (void)m.add_output(b);
    LUMEN_CHECK(same(m.slot_state(1).logical_rect(), {1918, 160, 1024, 768}));
    // A suggestion that would overlap (a 2x scale makes Virtual-1 narrower; the host's
    // pixel layout does not know) falls back to the automatic place.
    output_manager o;
    output_desc c = b;
    c.suggested_x = 100;
    c.suggested_y = 0;
    (void)o.add_output(a);
    (void)o.add_output(c);
    LUMEN_CHECK(same(o.slot_state(1).logical_rect(), {1918, 0, 1024, 768}));
}

LUMEN_TEST(layout_pointer_union_clamp_and_bounds) {
    output_manager m;
    // An L: A 1000x800 at 0,0; B 600x400 at 1000,0 (shorter).
    (void)add(m, "A", 1000, 800);
    (void)add(m, "B", 600, 400);
    LUMEN_CHECK(same(lx::compositor::layout_bounds(m), {0, 0, 1600, 800}));
    double x = 500, y = 500;
    LUMEN_CHECK(lx::compositor::closest_point(m, x, y) && x == 500 && y == 500); // on A
    x = 1200, y = 300;
    LUMEN_CHECK(lx::compositor::closest_point(m, x, y) && x == 1200 && y == 300); // on B
    // Below B, beside A: the gap. The nearest point is B's bottom edge.
    x = 1300, y = 600;
    LUMEN_CHECK(lx::compositor::closest_point(m, x, y) && x == 1300 && y == 399);
    // Past the right edge: clamped to B's last column.
    x = 2000, y = 100;
    LUMEN_CHECK(lx::compositor::closest_point(m, x, y) && x == 1599 && y == 100);
    // Far into the corner below B: A's corner is nearer than B's? (1000,800) vs (1599,399):
    // from (1700, 900) B's corner is nearer.
    x = 1700, y = 900;
    LUMEN_CHECK(lx::compositor::closest_point(m, x, y) && x == 1599 && y == 399);
    x = -50, y = 900;
    LUMEN_CHECK(lx::compositor::closest_point(m, x, y) && x == 0 && y == 799);
    LUMEN_CHECK(lx::compositor::output_at(m, 999.5, 10) == 0);
    LUMEN_CHECK(lx::compositor::output_at(m, 1000, 10) == 1);
    LUMEN_CHECK(lx::compositor::output_at(m, 1000, 500) == -1);
    output_manager none;
    x = 5, y = 5;
    LUMEN_CHECK(!lx::compositor::closest_point(none, x, y));
}

LUMEN_TEST(layout_pace_output) {
    output_manager m;
    (void)add(m, "A", 1000, 800);
    (void)add(m, "B", 1000, 800);
    const lx::rect2i straddle{900, 0, 400, 100}; // 100 px on A, 300 on B
    LUMEN_CHECK(lx::compositor::pace_output(m, 0b11, straddle) == 1);
    // Shown only on A (B's view covered it): A paces it.
    LUMEN_CHECK(lx::compositor::pace_output(m, 0b01, straddle) == 0);
    // 10 % or less does not count while another output shows more than that.
    const lx::rect2i sliver{990, 0, 200, 100}; // 10 px on A (5 %), 190 on B
    LUMEN_CHECK(lx::compositor::pace_output(m, 0b11, sliver) == 1);
    // ... unless it is all there is.
    LUMEN_CHECK(lx::compositor::pace_output(m, 0b01, sliver) == 0);
    // A tie goes to the lower slot; nothing shown: none.
    LUMEN_CHECK(lx::compositor::pace_output(m, 0b11, {900, 0, 200, 100}) == 0);
    LUMEN_CHECK(lx::compositor::pace_output(m, 0, straddle) == -1);
}

LUMEN_TEST(layout_default_scale_sway_rule) {
    using lx::compositor::compute_default_scale;
    // 27" 4K: 163 DPI — not HiDPI by sway's rule (fractional defaults are not guessed).
    LUMEN_CHECK(compute_default_scale(3840, 2160, 600, 340) == 1.0);
    // 13.3" 4K laptop panel: 332 DPI on both axes, 2160 rows — 2.
    LUMEN_CHECK(compute_default_scale(3840, 2160, 294, 165) == 2.0);
    // Dense but under 1200 rows: 1.
    LUMEN_CHECK(compute_default_scale(1920, 1080, 120, 68) == 1.0);
    // Unknown, or an aspect ratio in disguise (projectors, TVs): 1.
    LUMEN_CHECK(compute_default_scale(3840, 2160, 0, 0) == 1.0);
    LUMEN_CHECK(compute_default_scale(3840, 2160, 160, 90) == 1.0);
    // --scale wins over the rule; a per-connector override over --scale.
    output_manager m;
    m.set_scale_override("eDP-1", 1.25);
    output_desc d{};
    d.name = "eDP-1";
    d.mode = {3840, 2160};
    d.phys_w_mm = 294;
    d.phys_h_mm = 165;
    LUMEN_CHECK(m.add_output(d) == 0 && m.slot_state(0).scale == 1.25);
    d.name = "DP-1";
    LUMEN_CHECK(m.add_output(d) == 1 && m.slot_state(1).scale == 2.0);
    m.set_default_scale(1.5);
    d.name = "DP-2";
    LUMEN_CHECK(m.add_output(d) == 2 && m.slot_state(2).scale == 1.5);
}

LUMEN_TEST(layout_identifiers) {
    output_manager m;
    output_desc d{};
    d.name = "DP-1";
    d.mode = {2560, 1440};
    d.edid = true;
    d.make = "Dell Inc.";
    d.model = "DELL U2720Q";
    d.serial = "7X8S1234";
    (void)m.add_output(d);
    LUMEN_CHECK(std::strcmp(m.slot_state(0).identifier, "Dell Inc. DELL U2720Q 7X8S1234") == 0);
    LUMEN_CHECK(std::strcmp(m.slot_state(0).description, "Dell Inc. DELL U2720Q 7X8S1234 (DP-1)") == 0);
    // An identical monitor without a serial, twice: the second gets its connector appended.
    d.serial = "";
    d.name = "DP-2";
    (void)m.add_output(d);
    d.name = "DP-3";
    (void)m.add_output(d);
    LUMEN_CHECK(std::strcmp(m.slot_state(1).identifier, "Dell Inc. DELL U2720Q Unknown") == 0);
    LUMEN_CHECK(std::strcmp(m.slot_state(2).identifier,
                            "Dell Inc. DELL U2720Q Unknown connector:DP-3") == 0);
    // No EDID: the connector.
    output_desc v{};
    v.name = "Virtual-1";
    v.mode = {1918, 928};
    (void)m.add_output(v);
    LUMEN_CHECK(std::strcmp(m.slot_state(3).identifier, "connector:Virtual-1") == 0);
    LUMEN_CHECK(std::strcmp(m.slot_state(3).description, "Virtual-1") == 0);
    // Ids are never reused, slots are.
    const lx::output_id old = m.slot_state(0).id;
    m.remove_slot(0);
    (void)m.add_output(v);
    LUMEN_CHECK(m.slot_live(0) && !(m.slot_state(0).id == old));
}

LUMEN_TEST(layout_from_a_connector_scan) {
    lx::drm::connector_scan scan{};
    scan.count = 3;
    auto& a = scan.entries[0];
    std::snprintf(a.name, sizeof(a.name), "Virtual-1");
    a.connector_id = 44;
    a.status = lx::drm::connector_status::connected;
    a.mode_count = 2;
    a.modes[0] = {1024, 768, 60000, false};
    a.modes[1] = {1918, 928, 59999, true};
    a.preferred = 1;
    a.possible_crtcs = 1;
    auto& b = scan.entries[1];
    std::snprintf(b.name, sizeof(b.name), "Virtual-2");
    b.status = lx::drm::connector_status::disconnected;
    auto& c = scan.entries[2];
    std::snprintf(c.name, sizeof(c.name), "DP-1");
    c.status = lx::drm::connector_status::connected;
    c.non_desktop = true; // a VR headset: never part of the desktop
    c.mode_count = 1;
    c.modes[0] = {2160, 2160, 90000, true};
    output_manager m;
    m.configure_from_scan(scan);
    LUMEN_CHECK(m.count() == 1);
    const auto& o = m.slot_state(0);
    LUMEN_CHECK(std::strcmp(o.name, "Virtual-1") == 0 && o.connector_id == 44);
    LUMEN_CHECK(o.mode.width == 1918 && o.mode.height == 928 && o.refresh_mhz == 59999);
    LUMEN_CHECK(o.kms_mode.width == 1918 && o.possible_crtcs == 1 && !o.virtual_output);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
