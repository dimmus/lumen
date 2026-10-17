// The hotplug diff (`lx.compositor:output_policy` `diff_scan`, ADR 008 §B.2) against fake
// connector scans: connect, disconnect, reconnect, a preferred-mode change (the VMware window
// resize), a mode list change that keeps the preferred mode, Bad link-status, a moved host
// suggestion, another monitor on the same connector, connectors that make no output, slots
// running out — and an identical scan, which must give nothing and move no generation (§B.7).
#include "lumen_test.hpp"
#include "scan_builder.hpp"

import lx.foundation;
import lx.drm;
import lx.compositor;

using lx::compositor::diff_scan;
using lx::compositor::output_change;
using lx::compositor::output_changes;
using lx::compositor::output_manager;
using scan_builder::connector;
using scan_builder::mode;

namespace {

lx::drm::connector_scan two_virtual() {
    lx::drm::connector_scan s{};
    connector(s, 44, "Virtual-1", true, {mode(1918, 928), mode(1280, 800), mode(1024, 768)});
    connector(s, 53, "Virtual-2", false);
    return s;
}

unsigned count(const output_changes& c, output_change::kind k) {
    unsigned n = 0;
    for (unsigned i = 0; i < c.count; ++i)
        n += c.items[i].what == k ? 1u : 0u;
    return n;
}

const output_change* first(const output_changes& c, output_change::kind k) {
    for (unsigned i = 0; i < c.count; ++i)
        if (c.items[i].what == k)
            return &c.items[i];
    return nullptr;
}

} // namespace

LUMEN_TEST(scan_identical_is_nothing) {
    const auto s = two_virtual();
    output_manager m;
    m.configure_from_scan(s);
    LUMEN_CHECK(m.count() == 1);
    const unsigned gen = m.generation();
    const unsigned lgen = m.layout_generation();
    output_changes c{};
    diff_scan(m, s, c);
    LUMEN_CHECK(c.empty());
    LUMEN_CHECK(m.generation() == gen && m.layout_generation() == lgen);
}

LUMEN_TEST(scan_connect_and_disconnect) {
    auto s = two_virtual();
    output_manager m;
    m.configure_from_scan(s);
    connector(s, 53, "Virtual-2", true, {mode(1024, 768)});
    output_changes c{};
    diff_scan(m, s, c);
    LUMEN_CHECK(c.count == 1);
    const output_change* a = first(c, output_change::kind::added);
    LUMEN_CHECK(a && a->entry == 1);
    // The new one in the manager, then unplugged again: removed.
    m.add_output(lx::compositor::desc_from_probe(s.entries[1], s.entries[1].modes[0]));
    connector(s, 53, "Virtual-2", false);
    diff_scan(m, s, c);
    LUMEN_CHECK(c.count == 1);
    const output_change* r = first(c, output_change::kind::removed);
    LUMEN_CHECK(r && r->slot == 1);
    // The connector object itself gone (an MST port): removed too.
    connector(s, 53, "Virtual-2", true, {mode(1024, 768)});
    lx::drm::connector_scan gone{};
    connector(gone, 44, "Virtual-1", true, {mode(1918, 928), mode(1280, 800), mode(1024, 768)});
    diff_scan(m, gone, c);
    LUMEN_CHECK(count(c, output_change::kind::removed) == 1 && c.count == 1);
}

LUMEN_TEST(scan_preferred_change_is_the_vm_resize) {
    auto s = two_virtual();
    output_manager m;
    m.configure_from_scan(s);
    connector(s, 44, "Virtual-1", true, {mode(1600, 900), mode(1280, 800), mode(1024, 768)});
    output_changes c{};
    diff_scan(m, s, c);
    LUMEN_CHECK(c.count == 1);
    const output_change* ch = first(c, output_change::kind::changed);
    LUMEN_CHECK(ch && ch->slot == 0 && ch->entry == 0);
    LUMEN_CHECK(ch->preferred_changed && ch->modes_changed && !ch->link_bad);
    // Following the preferred mode: the new one.
    const auto* chosen = lx::compositor::choose_mode(s.entries[0], &m.slot_state(0));
    LUMEN_CHECK(chosen && chosen->width == 1600 && chosen->height == 900);
    // The manager takes it: generations move, the logical size follows.
    const unsigned gen = m.generation();
    m.set_mode(0, *chosen, true);
    m.update_head(0, s.entries[0]);
    LUMEN_CHECK(m.generation() != gen);
    LUMEN_CHECK(m.slot_state(0).logical_rect().width == 1600);
    diff_scan(m, s, c);
    LUMEN_CHECK(c.empty());
}

LUMEN_TEST(scan_mode_list_change_keeps_preferred) {
    auto s = two_virtual();
    output_manager m;
    m.configure_from_scan(s);
    connector(s, 44, "Virtual-1", true, {mode(1918, 928), mode(800, 600)});
    output_changes c{};
    diff_scan(m, s, c);
    const output_change* ch = first(c, output_change::kind::changed);
    LUMEN_CHECK(ch && ch->modes_changed && !ch->preferred_changed);
    // The current mode is still the choice: no modeset needed.
    const auto* chosen = lx::compositor::choose_mode(s.entries[0], &m.slot_state(0));
    LUMEN_CHECK(chosen && chosen->same_timings(m.slot_state(0).kms_mode));
}

LUMEN_TEST(scan_link_bad_and_suggested_position) {
    auto s = two_virtual();
    output_manager m;
    m.configure_from_scan(s);
    s.entries[0].link_bad = true;
    output_changes c{};
    diff_scan(m, s, c);
    LUMEN_CHECK(c.count == 1 && c.items[0].link_bad && !c.items[0].preferred_changed);
    s.entries[0].link_bad = false;
    s.entries[0].hotplug_mode_update = true;
    s.entries[0].suggested_x = 0;
    s.entries[0].suggested_y = 0;
    diff_scan(m, s, c);
    LUMEN_CHECK(c.count == 1 && c.items[0].head_changed && !c.items[0].modes_changed);
    m.update_head(0, s.entries[0]);
    diff_scan(m, s, c);
    LUMEN_CHECK(c.empty());
}

LUMEN_TEST(scan_other_monitor_on_same_connector) {
    lx::drm::connector_scan s{};
    connector(s, 70, "DP-1", true, {mode(2560, 1440)}, {"DEL", "U2720Q", "AB1"});
    output_manager m;
    m.configure_from_scan(s);
    LUMEN_CHECK(std::strcmp(m.slot_state(0).identifier, "DEL U2720Q AB1") == 0);
    // Unplugged and another plugged in between two probes: one goes, one comes.
    connector(s, 70, "DP-1", true, {mode(1920, 1080)}, {"GSM", "LG", "Z9"});
    output_changes c{};
    diff_scan(m, s, c);
    LUMEN_CHECK(count(c, output_change::kind::removed) == 1);
    LUMEN_CHECK(count(c, output_change::kind::added) == 1);
}

LUMEN_TEST(scan_connectors_that_make_no_output) {
    lx::drm::connector_scan s{};
    connector(s, 44, "Virtual-1", true, {mode(1024, 768)});
    output_manager m;
    m.configure_from_scan(s);
    // A VR headset (non-desktop) and a connected connector with no mode: nothing.
    connector(s, 80, "DP-3", true, {mode(2160, 2160)}).non_desktop = true;
    connector(s, 81, "DP-4", true, {});
    output_changes c{};
    diff_scan(m, s, c);
    LUMEN_CHECK(c.empty());
    // Outputs without a connector (HEADLESS-n) are not the scan's.
    m.add_virtual("HEADLESS-2", {0, 0, 800, 600}, 60000);
    diff_scan(m, s, c);
    LUMEN_CHECK(c.empty());
}

LUMEN_TEST(scan_slots_run_out) {
    lx::drm::connector_scan s{};
    char name[16];
    for (unsigned i = 0; i < 15; ++i) {
        std::snprintf(name, sizeof(name), "Virtual-%u", i + 1);
        connector(s, 100 + i, name, true, {mode(640, 480)});
    }
    output_manager m;
    m.configure_from_scan(s);
    m.add_virtual("HEADLESS-9", {0, 0, 640, 480}, 60000);
    LUMEN_CHECK(m.count() == output_manager::k_max);
    connector(s, 200, "Virtual-16", true, {mode(640, 480)});
    output_changes c{};
    diff_scan(m, s, c);
    LUMEN_CHECK(c.count == 0 && c.dropped == 1);
    // One going at the same time makes room.
    connector(s, 100, "Virtual-1", false);
    diff_scan(m, s, c);
    LUMEN_CHECK(count(c, output_change::kind::removed) == 1);
    LUMEN_CHECK(count(c, output_change::kind::added) == 1 && c.dropped == 0);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
