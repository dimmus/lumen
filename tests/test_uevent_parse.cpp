// Hotplug uevents (`lx.drm:uevent`, ADR 008 H1): which uevents start a connector probe and
// what they ask it to cover. Only `change` with HOTPLUG=1 of our card is a hotplug; a
// CONNECTOR= hint narrows the probe to that connector, no hint (vmwgfx's layout update)
// means every connector; `remove` of the card is the card going; anything else — another
// card, a render node, `udevadm trigger` without HOTPLUG — is nothing. A burst folds into
// one batch: hints deduplicated, past eight (or after a hint-less event) all connectors.
#include "lumen_test.hpp"

#include <cstdio>

import lx.drm;

using lx::drm::hotplug_batch;
using lx::drm::merge_hotplug;
using lx::drm::parse_uevent;

namespace {
constexpr unsigned long long k_card = (226ull << 8) | 0; // 226:0
constexpr unsigned long long k_render = (226ull << 8) | 128;
} // namespace

LUMEN_TEST(uevent_hotplug_without_hint_probes_all) {
    const hotplug_batch b = parse_uevent("change", k_card, k_card, "1", nullptr, nullptr, nullptr);
    LUMEN_CHECK(b.hotplug);
    LUMEN_CHECK(b.all_connectors);
    LUMEN_CHECK(b.connector_count == 0);
    LUMEN_CHECK(!b.card_removed && !b.lease);
    LUMEN_CHECK(!b.empty());
}

LUMEN_TEST(uevent_connector_hint) {
    const hotplug_batch b = parse_uevent("change", k_card, k_card, "1", "44", "61", nullptr);
    LUMEN_CHECK(b.hotplug);
    LUMEN_CHECK(!b.all_connectors);
    LUMEN_CHECK(b.connector_count == 1 && b.connectors[0] == 44);
    // A malformed hint is no hint.
    const hotplug_batch bad = parse_uevent("change", k_card, k_card, "1", "4x", nullptr, nullptr);
    LUMEN_CHECK(bad.hotplug && bad.all_connectors);
}

LUMEN_TEST(uevent_ignored) {
    // A change without HOTPLUG=1 (udevadm trigger), HOTPLUG=0, another device, other actions.
    LUMEN_CHECK(parse_uevent("change", k_card, k_card, nullptr, nullptr, nullptr, nullptr).empty());
    LUMEN_CHECK(parse_uevent("change", k_card, k_card, "0", nullptr, nullptr, nullptr).empty());
    LUMEN_CHECK(parse_uevent("change", k_render, k_card, "1", nullptr, nullptr, nullptr).empty());
    LUMEN_CHECK(parse_uevent("add", k_card, k_card, "1", nullptr, nullptr, nullptr).empty());
    LUMEN_CHECK(parse_uevent("bind", k_card, k_card, nullptr, nullptr, nullptr, nullptr).empty());
    LUMEN_CHECK(parse_uevent(nullptr, k_card, k_card, "1", nullptr, nullptr, nullptr).empty());
    // No card known: nothing is ours.
    LUMEN_CHECK(parse_uevent("change", 0, 0, "1", nullptr, nullptr, nullptr).empty());
    // Every one is still counted.
    LUMEN_CHECK(parse_uevent("change", k_render, k_card, "1", nullptr, nullptr, nullptr).events == 1);
}

LUMEN_TEST(uevent_card_removed_and_lease) {
    const hotplug_batch r = parse_uevent("remove", k_card, k_card, nullptr, nullptr, nullptr, nullptr);
    LUMEN_CHECK(r.card_removed && !r.hotplug);
    LUMEN_CHECK(!parse_uevent("remove", k_render, k_card, nullptr, nullptr, nullptr, nullptr).card_removed);
    const hotplug_batch l = parse_uevent("change", k_card, k_card, nullptr, nullptr, nullptr, "1");
    LUMEN_CHECK(l.lease && !l.hotplug);
}

LUMEN_TEST(uevent_burst_folds) {
    hotplug_batch b{};
    merge_hotplug(b, parse_uevent("change", k_card, k_card, "1", "44", nullptr, nullptr));
    merge_hotplug(b, parse_uevent("change", k_card, k_card, "1", "44", nullptr, nullptr));
    merge_hotplug(b, parse_uevent("change", k_card, k_card, "1", "53", nullptr, nullptr));
    merge_hotplug(b, parse_uevent("change", k_render, k_card, "1", nullptr, nullptr, nullptr));
    LUMEN_CHECK(b.hotplug && !b.all_connectors);
    LUMEN_CHECK(b.connector_count == 2);
    LUMEN_CHECK(b.connectors[0] == 44 && b.connectors[1] == 53);
    LUMEN_CHECK(b.events == 4);
    // A hint-less one makes it all.
    merge_hotplug(b, parse_uevent("change", k_card, k_card, "1", nullptr, nullptr, nullptr));
    LUMEN_CHECK(b.all_connectors && b.connector_count == 0);
    // Hints after that change nothing.
    merge_hotplug(b, parse_uevent("change", k_card, k_card, "1", "62", nullptr, nullptr));
    LUMEN_CHECK(b.all_connectors && b.connector_count == 0);
}

LUMEN_TEST(uevent_more_hints_than_fit) {
    hotplug_batch b{};
    char id[8];
    for (unsigned i = 0; i < hotplug_batch::k_max_hints; ++i) {
        std::snprintf(id, sizeof(id), "%u", 100 + i);
        merge_hotplug(b, parse_uevent("change", k_card, k_card, "1", id, nullptr, nullptr));
    }
    LUMEN_CHECK(!b.all_connectors && b.connector_count == hotplug_batch::k_max_hints);
    merge_hotplug(b, parse_uevent("change", k_card, k_card, "1", "999", nullptr, nullptr));
    LUMEN_CHECK(b.all_connectors && b.connector_count == 0);
}

LUMEN_TEST(uevent_monitor_open_without_card) {
    // No card dev_t: refused with a typed error (or not_implemented without libudev).
    auto m = lx::drm::uevent_monitor::open(0);
    LUMEN_CHECK(!m);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
