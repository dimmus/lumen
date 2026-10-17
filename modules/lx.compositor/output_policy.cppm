module;

// Output policy for hotplug (ADR 008 §B.2): what a new connector probe means for the outputs
// — which appeared, which went, which changed — and what mode each one takes. Pure (the
// output manager and a scan in, decisions out), so it is unit-tested with fake scans.
//
// Identity is the connector (its KMS object id): a connector that stays connected is the same
// output, even when its monitor changes; a different EDID identity on it is reported as the
// old output going and a new one coming (wlroots and KWin destroy and recreate the output
// then too). Mode choice (H3): an output that follows its connector's preferred mode takes
// the new preferred mode — the VMware window resize — and one with an explicit mode keeps it
// while the connector still offers it, falling back to preferred otherwise (niri, sway's
// search). A Bad link-status retrains the link with a modeset of the same mode (wlroots,
// KWin). The fallback order when a mode fails its TEST_ONLY is sway's `search_valid_config`:
// the chosen mode, the preferred one, then the rest by area × refresh.

#include <cstdint>
#include <cstring>

import lx.foundation;
import lx.drm;

export module lx.compositor:output_policy;

import :output;

export namespace lx::compositor {

struct output_change {
    enum class kind : unsigned char { added, removed, changed };
    kind what = kind::changed;
    /// removed / changed: the output's slot; added: -1.
    int slot = -1;
    /// added / changed: the connector's index in the scan; removed: -1.
    int entry = -1;
    // changed: what moved.
    /// The connector's preferred mode is another mode now.
    bool preferred_changed = false;
    /// The mode list differs (it may still hold the current mode).
    bool modes_changed = false;
    /// `link-status` is Bad: a modeset retrains the link.
    bool link_bad = false;
    /// The host's suggested position, physical size or routing hints differ.
    bool head_changed = false;
};

struct output_changes {
    static constexpr unsigned k_max = 2 * output_manager::k_max;
    output_change items[k_max]{};
    unsigned count = 0;
    /// Connected desktop connectors that found no free slot (past `output_manager::k_max`).
    unsigned dropped = 0;

    [[nodiscard]] bool empty() const { return count == 0; }
};

/// The diff of `scan` against the outputs (ADR 008 §B.2). Outputs without a connector
/// (HEADLESS-n) are not the scan's and never appear. Identical state gives `{}`.
void diff_scan(const output_manager& outputs, const lx::drm::connector_scan& scan,
               output_changes& out);

/// The mode an output on connector `probe` takes (H3). `current` is the output's state, null
/// for a new output: following the preferred mode → the preferred mode; an explicit mode →
/// the same timings when still offered, else the preferred mode. Null without any mode.
[[nodiscard]] const lx::drm::kms_mode* choose_mode(const lx::drm::connector_probe& probe,
                                                   const output_state* current);

/// The modes to try, in order, when the first fails its TEST_ONLY (sway's search order):
/// `first`, then the preferred mode, then every other mode by area × refresh, descending,
/// without repeating timings. Returns how many were written to `out` (at most `max`).
unsigned mode_candidates(const lx::drm::connector_probe& probe, const lx::drm::kms_mode* first,
                         lx::drm::kms_mode* out, unsigned max);

/// The output a window or the pointer falls back to when output `going` is removed (§B.5):
/// the one under the pointer (`pointer_slot`) unless it is the one going, else the first
/// remaining in layout order (leftmost, then topmost). -1 when no other output is live.
[[nodiscard]] int fallback_output(const output_manager& outputs, unsigned going,
                                  int pointer_slot);

} // namespace lx::compositor

namespace {

[[nodiscard]] const lx::drm::connector_probe* find_entry(const lx::drm::connector_scan& scan,
                                                       unsigned connector_id, int& index) {
    for (unsigned i = 0; i < scan.count; ++i) {
        if (scan.entries[i].connector_id == connector_id) {
            index = static_cast<int>(i);
            return &scan.entries[i];
        }
    }
    index = -1;
    return nullptr;
}

/// The identity of the monitor on `probe` as `make_identity` derives it (before duplicates
/// get their connector appended): "make model serial" with "Unknown" for missing parts, or
/// the connector without EDID.
[[nodiscard]] bool same_monitor(const lx::compositor::output_state& o,
                                const lx::drm::connector_probe& e) {
    if (o.edid != e.edid)
        return false;
    if (!e.edid)
        return std::strcmp(o.name, e.name) == 0;
    return std::strcmp(o.make, e.make) == 0 && std::strcmp(o.model, e.model) == 0 &&
           std::strcmp(o.serial, e.serial) == 0;
}

} // namespace

void lx::compositor::diff_scan(const output_manager& outputs, const lx::drm::connector_scan& scan,
                               output_changes& out) {
    out.count = 0;
    out.dropped = 0;
    const auto push = [&out](const output_change& c) {
        if (out.count < output_changes::k_max)
            out.items[out.count++] = c;
    };
    unsigned live_connectors = 0;
    // Outputs the scan no longer backs (disconnected, gone from the resources, another
    // monitor on the connector) go; the rest are compared with their connector's probe.
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const output_state& o = outputs.slot_state(s);
        if (o.connector_id == 0)
            continue; // not a connector's (HEADLESS-n)
        int index = -1;
        const lx::drm::connector_probe* e = find_entry(scan, o.connector_id, index);
        if (!e || !desktop_connector(*e) || !same_monitor(o, *e)) {
            output_change c{};
            c.what = output_change::kind::removed;
            c.slot = static_cast<int>(s);
            push(c);
            continue;
        }
        ++live_connectors;
        output_change c{};
        c.what = output_change::kind::changed;
        c.slot = static_cast<int>(s);
        c.entry = index;
        const lx::drm::kms_mode* pm = e->preferred_mode();
        c.preferred_changed = pm && !pm->same_timings(o.preferred_mode);
        c.modes_changed = mode_list_hash(*e) != o.modes_hash;
        c.link_bad = e->link_bad;
        c.head_changed = e->suggested_x != o.suggested_x || e->suggested_y != o.suggested_y ||
                         e->hotplug_mode_update != o.hotplug_mode_update ||
                         static_cast<int>(e->phys_w_mm) != o.phys_w_mm ||
                         static_cast<int>(e->phys_h_mm) != o.phys_h_mm ||
                         e->possible_crtcs != o.possible_crtcs;
        if (c.preferred_changed || c.modes_changed || c.link_bad || c.head_changed)
            push(c);
    }
    // Connected connectors no output stands for: new outputs (a monitor swapped on a
    // connector was removed above, so it is added here).
    unsigned free_slots = output_manager::k_max - outputs.count();
    for (unsigned i = 0; i < scan.count; ++i) {
        const lx::drm::connector_probe& e = scan.entries[i];
        if (!desktop_connector(e))
            continue;
        bool known = false;
        for (unsigned s = 0; s < output_manager::k_max && !known; ++s) {
            if (!outputs.slot_live(s))
                continue;
            const output_state& o = outputs.slot_state(s);
            known = o.connector_id == e.connector_id && same_monitor(o, e);
        }
        if (known)
            continue;
        // A removed output's slot is free again once it is gone; count those too.
        if (free_slots == 0) {
            bool reuse = false;
            for (unsigned k = 0; k < out.count && !reuse; ++k)
                reuse = out.items[k].what == output_change::kind::removed;
            if (!reuse) {
                ++out.dropped;
                continue;
            }
        } else {
            --free_slots;
        }
        output_change c{};
        c.what = output_change::kind::added;
        c.entry = static_cast<int>(i);
        push(c);
    }
    (void)live_connectors;
}

const lx::drm::kms_mode* lx::compositor::choose_mode(const lx::drm::connector_probe& e,
                                                     const output_state* current) {
    const lx::drm::kms_mode* preferred = e.preferred_mode();
    if (!current || current->follow_preferred)
        return preferred;
    for (unsigned i = 0; i < e.mode_count; ++i)
        if (e.modes[i].same_timings(current->kms_mode))
            return &e.modes[i];
    return preferred;
}

unsigned lx::compositor::mode_candidates(const lx::drm::connector_probe& e,
                                         const lx::drm::kms_mode* first, lx::drm::kms_mode* out,
                                         unsigned max) {
    unsigned n = 0;
    const auto add = [&](const lx::drm::kms_mode& m) {
        if (n >= max || !m.valid())
            return;
        for (unsigned k = 0; k < n; ++k)
            if (out[k].same_timings(m))
                return;
        out[n++] = m;
    };
    if (first)
        add(*first);
    if (const lx::drm::kms_mode* pm = e.preferred_mode())
        add(*pm);
    // The rest by area × refresh, descending; ties keep the driver's order. Selection by
    // repeated maximum: at most `max` passes over ≤ 96 modes, no allocation.
    bool used[lx::drm::k_max_modes]{};
    while (n < max) {
        int best = -1;
        unsigned long long best_key = 0;
        for (unsigned i = 0; i < e.mode_count; ++i) {
            if (used[i] || !e.modes[i].valid())
                continue;
            const unsigned long long key = static_cast<unsigned long long>(e.modes[i].width) *
                                           e.modes[i].height * e.modes[i].refresh_mhz;
            if (best < 0 || key > best_key) {
                best = static_cast<int>(i);
                best_key = key;
            }
        }
        if (best < 0)
            break;
        used[best] = true;
        add(e.modes[best]);
    }
    return n;
}

int lx::compositor::fallback_output(const output_manager& outputs, unsigned going,
                                    int pointer_slot) {
    if (pointer_slot >= 0 && static_cast<unsigned>(pointer_slot) != going &&
        outputs.slot_live(static_cast<unsigned>(pointer_slot)))
        return pointer_slot;
    int first = -1;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (s == going || !outputs.slot_live(s))
            continue;
        if (first < 0) {
            first = static_cast<int>(s);
            continue;
        }
        const lx::rect2i l = outputs.slot_state(s).logical_rect();
        const lx::rect2i f = outputs.slot_state(static_cast<unsigned>(first)).logical_rect();
        if (l.x < f.x || (l.x == f.x && l.y < f.y))
            first = static_cast<int>(s);
    }
    return first;
}
