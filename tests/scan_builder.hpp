// Fake connector scans for hotplug tests (ADR 008 §D.1): what `kms_device::probe_connectors`
// would produce, built by hand — no hardware.
#pragma once

#include <cstdio>
#include <cstring>
#include <initializer_list>

import lx.drm;

namespace scan_builder {

inline lx::drm::kms_mode mode(unsigned w, unsigned h, unsigned mhz = 60000, bool preferred = false) {
    lx::drm::kms_mode m{};
    m.width = w;
    m.height = h;
    m.refresh_mhz = mhz;
    m.preferred = preferred;
    // Distinct timings per size and refresh: what `same_timings` compares.
    const unsigned stamp[3] = {w, h, mhz};
    std::memcpy(m.kms.bytes, stamp, sizeof(stamp));
    return m;
}

struct monitor {
    const char* make = nullptr; // null: no EDID
    const char* model = "";
    const char* serial = "";
};

/// Appends (or replaces, by id) connector `id` named `name`: connected with `modes` (the
/// first flagged preferred unless one already is), or disconnected without modes.
inline lx::drm::connector_probe& connector(lx::drm::connector_scan& s, unsigned id, const char* name,
                                           bool connected,
                                           std::initializer_list<lx::drm::kms_mode> modes = {},
                                           monitor mon = {}) {
    lx::drm::connector_probe* e = nullptr;
    for (unsigned i = 0; i < s.count; ++i)
        if (s.entries[i].connector_id == id)
            e = &s.entries[i];
    if (!e)
        e = &s.entries[s.count++];
    *e = {};
    e->connector_id = id;
    std::snprintf(e->name, sizeof(e->name), "%s", name);
    e->status = connected ? lx::drm::connector_status::connected
                          : lx::drm::connector_status::disconnected;
    e->possible_crtcs = 1u << (id % 8);
    if (mon.make) {
        e->edid = true;
        std::snprintf(e->make, sizeof(e->make), "%s", mon.make);
        std::snprintf(e->model, sizeof(e->model), "%s", mon.model);
        std::snprintf(e->serial, sizeof(e->serial), "%s", mon.serial);
    }
    if (!connected)
        return *e;
    bool any_preferred = false;
    for (const auto& m : modes)
        any_preferred = any_preferred || m.preferred;
    for (const auto& m : modes) {
        e->modes[e->mode_count] = m;
        if (!any_preferred && e->mode_count == 0)
            e->modes[0].preferred = true;
        if (e->modes[e->mode_count].preferred && e->preferred < 0)
            e->preferred = static_cast<int>(e->mode_count);
        ++e->mode_count;
    }
    return *e;
}

} // namespace scan_builder
