// lumen-settings' output management (ADR-013 Phase F): the client half of
// zwlr_output_manager_v1 — the same global wlr-randr and kanshi drive, non-privileged for
// same-UID clients. Heads arrive through a second wl_registry (lx.shell's pattern); the
// compositor validates every configuration with a test, applies it and persists
// outputs.toml itself (ADR-008 C1–C5) — this client only stages and asks.
module;
#ifdef LUMEN_SETTINGS_HAS_OUTPUT_MANAGEMENT
#include <wayland-client.h>
#include "wlr-output-management-unstable-v1-client-protocol.h"
#endif

export module lumen.settings_ui:displays;

import std;
import lx.foundation;

export namespace settings {

/// One output head, as the Displays pane shows and stages it.
struct display_head {
    std::string name;        ///< the connector ("DP-1")
    std::string description; ///< the monitor string
    bool enabled = true;
    struct mode_info {
        int w = 0, h = 0;
        unsigned refresh_mhz = 0;
        bool preferred = false;
        bool current = false;
        void* proxy = nullptr; ///< zwlr_output_mode_v1*
    };
    std::vector<mode_info> modes;
    void* proxy = nullptr;        ///< zwlr_output_head_v1*
    int w = 0, h = 0;             ///< the current mode
    unsigned refresh_mhz = 0;
    double scale = 1.0;
    std::string transform = "normal";
    int x = 0, y = 0;             ///< the logical position
    bool adaptive_sync = false;
    /// Staged edits (what Apply sends; the pane shows a modified dot).
    bool staged = false;
    bool staged_enabled = false;
    int staged_mode = -1;         ///< index into modes
    double staged_scale = 1.0;
    std::string staged_transform;
    int staged_x = 0, staged_y = 0;
    bool staged_adaptive = false;
};

/// True while the compositor's output-management global is present (a sandboxed client
/// never sees it; neither does an old compositor).
[[nodiscard]] bool outputs_available();
/// The heads as of the last done event (UI affinity).
[[nodiscard]] const std::vector<display_head>& output_heads();
/// The last apply's or event's note ("" clean).
[[nodiscard]] std::string_view outputs_note();
/// Stages an edit on head `i` (the pane's controls call these; each repaints).
void stage_enabled(unsigned i, bool on);
void stage_mode(unsigned i, int mode_index);
void stage_scale(unsigned i, double scale);
void stage_transform(unsigned i, std::string_view transform);
void stage_position(unsigned i, int x, int y);
void stage_adaptive(unsigned i, bool on);
/// Sends the staged edits for head `i` as one configuration (the compositor tests, applies
/// and persists outputs.toml). The note says the outcome.
void apply_head(unsigned i);
/// Discards head `i`'s staged edits.
void discard_staged(unsigned i);
/// Starts listening (UI thread, after the display connects); `native_display` is the
/// app's wl_display*. `refresh` runs on every state change (one re-describe).
void start_outputs(void* native_display, void (*refresh)());
void stop_outputs();
/// One re-describe from outside (the pane's refresh hook).
void note_outputs_dirty();

} // namespace settings

// ── implementation ──────────────────────────────────────────────────────────────────

#ifdef LUMEN_SETTINGS_HAS_OUTPUT_MANAGEMENT

// Two constraints shape the client half of the protocol, and both are libwayland's.
//
// A listener's user-data has to outlive the proxy it was attached to, so it can never be the
// address of a vector element: heads and modes arrive one at a time, `std::vector` grows by
// reallocating, and every user-data pointer taken before the last `emplace_back` dangles —
// after which the next state event writes through freed memory. Heads and modes therefore
// live in fixed slot tables whose addresses never move, and a slot is released only when its
// object is. The pane's list is a projection of those slots, rebuilt when one changes.
//
// And a configuration names *every* head — a head left out is the protocol error
// `unconfigured_head`, which this compositor raises. `apply_head(i)` therefore sends one
// configuration that restates every other head exactly as it already is and head `i` as it
// was staged, so staging one output never reconfigures its neighbours.

namespace {

using namespace settings;

inline constexpr unsigned k_max_heads = 32;
inline constexpr unsigned k_mode_slots = 64;

/// One head's listener storage. Lives for the process, so a proxy's user-data never moves.
struct head_slot {
    bool live = false;
    display_head data{};
    display_head::mode_info modes[k_mode_slots]{};
    bool mode_live[k_mode_slots]{};
};

head_slot g_slots[k_max_heads]{};             // NOLINT(cppcoreguidelines-avoid-non-const-global-variable)
std::vector<display_head> g_view;             // NOLINT: the pane's projection of the slots
bool g_available = false;                     // NOLINT
std::string g_note;                           // NOLINT
wl_display* g_display = nullptr;              // NOLINT
wl_registry* g_registry = nullptr;            // NOLINT
zwlr_output_manager_v1* g_manager = nullptr;  // NOLINT
uint32_t g_manager_version = 0;               // what we bound; adaptive sync needs 4
uint32_t g_serial = 0;                        // the last done
zwlr_output_configuration_v1* g_inflight = nullptr; // one apply at a time
std::string g_inflight_name;                  // the head whose staged edits it carries
void (*g_refresh)() = nullptr;                // NOLINT

/// Rebuilds the pane's list from the slots, and tells the pane.
void dirty(const char* note = nullptr) {
    if (note)
        g_note = note;
    g_view.clear();
    for (const head_slot& s : g_slots) {
        if (!s.live)
            continue;
        display_head d = s.data;
        d.modes.clear();
        for (unsigned i = 0; i < k_mode_slots; ++i)
            if (s.mode_live[i])
                d.modes.push_back(s.modes[i]);
        g_view.push_back(std::move(d));
    }
    if (g_refresh)
        g_refresh();
}

[[nodiscard]] int transform_index(std::string_view name) {
    static constexpr std::string_view k_names[] = {"normal",     "90",         "180",
                                                   "270",        "flipped",    "flipped-90",
                                                   "flipped-180", "flipped-270"};
    for (unsigned i = 0; i < 8; ++i)
        if (k_names[i] == name)
            return static_cast<int>(i);
    return 0;
}

[[nodiscard]] const char* transform_name(int t) {
    static constexpr const char* k_names[] = {"normal",      "90",          "180",
                                              "270",         "flipped",     "flipped-90",
                                              "flipped-180", "flipped-270"};
    return t >= 0 && t < 8 ? k_names[t] : "normal";
}

/// The pane's index (declaration order among live heads) to its slot, or null.
[[nodiscard]] head_slot* slot_at(unsigned i) {
    unsigned n = 0;
    for (head_slot& s : g_slots) {
        if (!s.live)
            continue;
        if (n == i)
            return &s;
        ++n;
    }
    return nullptr;
}

/// The live mode slot that the pane's index `k` names (declaration order), or -1.
[[nodiscard]] int mode_slot_at(const head_slot& s, int k) {
    if (k < 0)
        return -1;
    int n = 0;
    for (unsigned i = 0; i < k_mode_slots; ++i) {
        if (!s.mode_live[i])
            continue;
        if (n == k)
            return static_cast<int>(i);
        ++n;
    }
    return -1;
}

/// The head's current mode as a pane index, or -1 when it has none (a disabled head).
[[nodiscard]] int current_mode_index(const head_slot& s) {
    int n = 0;
    for (unsigned i = 0; i < k_mode_slots; ++i) {
        if (!s.mode_live[i])
            continue;
        if (s.modes[i].current)
            return n;
        ++n;
    }
    return -1;
}

/// The proxy of the mode the pane's index names, when that object is still the head's.
[[nodiscard]] zwlr_output_mode_v1* mode_proxy(const head_slot& s, int pane_index) {
    const int k = mode_slot_at(s, pane_index);
    return k < 0 ? nullptr : static_cast<zwlr_output_mode_v1*>(s.modes[k].proxy);
}

/// Releases a slot's proxies and its state. The socket's objects: a mode or head the server
/// finished with must be destroyed here, and one the server never finished with (the manager
/// went away) still holds a client-side proxy that has to be freed.
void clear_slot(head_slot& s) {
    for (unsigned i = 0; i < k_mode_slots; ++i) {
        if (!s.mode_live[i])
            continue;
        s.mode_live[i] = false;
        if (s.modes[i].proxy)
            zwlr_output_mode_v1_destroy(static_cast<zwlr_output_mode_v1*>(s.modes[i].proxy));
        s.modes[i] = display_head::mode_info{};
    }
    if (s.data.proxy)
        zwlr_output_head_v1_destroy(static_cast<zwlr_output_head_v1*>(s.data.proxy));
    s = head_slot{};
}

// Forward: the head listener is registered before the manager's heads arrive; the mode
// listener is referenced by the head listener's mode event.
const zwlr_output_mode_v1_listener* mode_listener();
const zwlr_output_head_v1_listener* head_listener();

void handle_mode_size(void* data, zwlr_output_mode_v1*, int32_t w, int32_t h) {
    auto* m = static_cast<display_head::mode_info*>(data);
    m->w = w;
    m->h = h;
    dirty();
}

void handle_mode_refresh(void* data, zwlr_output_mode_v1*, int32_t mhz) {
    static_cast<display_head::mode_info*>(data)->refresh_mhz = static_cast<unsigned>(mhz);
    dirty();
}

void handle_mode_preferred(void* data, zwlr_output_mode_v1*) {
    static_cast<display_head::mode_info*>(data)->preferred = true;
    dirty();
}

void handle_mode_finished(void* data, zwlr_output_mode_v1* mode) {
    // Found by address, then marked free in place: the slot array never shifts, so no other
    // mode's user-data moves — compacting it here would be the same stale pointer one level
    // down. An id the head still reports as current is cleared with it.
    auto* m = static_cast<display_head::mode_info*>(data);
    for (head_slot& s : g_slots)
        for (unsigned i = 0; i < k_mode_slots; ++i)
            if (&s.modes[i] == m) {
                s.mode_live[i] = false;
                s.modes[i] = display_head::mode_info{};
                if (s.data.staged_mode >= 0)
                    s.data.staged_mode = current_mode_index(s);
            }
    zwlr_output_mode_v1_destroy(mode);
    dirty();
}

const zwlr_output_mode_v1_listener k_mode_listener = {
    .size = handle_mode_size,
    .refresh = handle_mode_refresh,
    .preferred = handle_mode_preferred,
    .finished = handle_mode_finished,
};

const zwlr_output_mode_v1_listener* mode_listener() { return &k_mode_listener; }

void handle_head_name(void* data, zwlr_output_head_v1*, const char* name) {
    static_cast<head_slot*>(data)->data.name = name;
    dirty();
}

void handle_head_description(void* data, zwlr_output_head_v1*, const char* d) {
    static_cast<head_slot*>(data)->data.description = d;
    dirty();
}

void handle_head_enabled(void* data, zwlr_output_head_v1*, int32_t on) {
    static_cast<head_slot*>(data)->data.enabled = on != 0;
    dirty();
}

void handle_head_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    auto* s = static_cast<head_slot*>(data);
    unsigned k = 0;
    while (k < k_mode_slots && s->mode_live[k])
        ++k;
    if (k == k_mode_slots) { // more modes than the pane shows: let the object go
        zwlr_output_mode_v1_destroy(mode);
        return;
    }
    s->modes[k] = display_head::mode_info{};
    s->modes[k].proxy = mode;
    s->mode_live[k] = true;
    zwlr_output_mode_v1_add_listener(mode, mode_listener(), &s->modes[k]);
}

void handle_head_scale(void* data, zwlr_output_head_v1*, wl_fixed_t f) {
    static_cast<head_slot*>(data)->data.scale = wl_fixed_to_double(f);
    dirty();
}

void handle_head_transform(void* data, zwlr_output_head_v1*, int32_t t) {
    static_cast<head_slot*>(data)->data.transform = transform_name(t);
    dirty();
}

void handle_head_position(void* data, zwlr_output_head_v1*, int32_t x, int32_t y) {
    auto& d = static_cast<head_slot*>(data)->data;
    d.x = x;
    d.y = y;
    dirty();
}

void handle_head_current_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    auto* s = static_cast<head_slot*>(data);
    for (unsigned i = 0; i < k_mode_slots; ++i) {
        if (!s->mode_live[i])
            continue;
        display_head::mode_info& m = s->modes[i];
        m.current = m.proxy == mode;
        if (m.current) {
            s->data.w = m.w;
            s->data.h = m.h;
            s->data.refresh_mhz = m.refresh_mhz;
        }
    }
    dirty();
}

void handle_head_adaptive_sync(void* data, zwlr_output_head_v1*, uint32_t state) {
    static_cast<head_slot*>(data)->data.adaptive_sync = state == 1; // enabled
    dirty();
}

void handle_head_finished(void* data, zwlr_output_head_v1*) {
    clear_slot(*static_cast<head_slot*>(data));
    dirty();
}

// physical_size, make, model and serial_number carry nothing the pane shows (the
// description already holds the monitor string), but their slots must be non-NULL —
// libwayland aborts the client on an event whose listener function is NULL.
void handle_head_physical_size(void*, zwlr_output_head_v1*, int32_t, int32_t) {}
void handle_head_make(void*, zwlr_output_head_v1*, const char*) {}
void handle_head_model(void*, zwlr_output_head_v1*, const char*) {}
void handle_head_serial_number(void*, zwlr_output_head_v1*, const char*) {}

// In declaration order (the wire's opcode order) with every event covered.
const zwlr_output_head_v1_listener k_head_listener = {
    .name = handle_head_name,
    .description = handle_head_description,
    .physical_size = handle_head_physical_size,
    .mode = handle_head_mode,
    .enabled = handle_head_enabled,
    .current_mode = handle_head_current_mode,
    .position = handle_head_position,
    .transform = handle_head_transform,
    .scale = handle_head_scale,
    .finished = handle_head_finished,
    .make = handle_head_make,
    .model = handle_head_model,
    .serial_number = handle_head_serial_number,
    .adaptive_sync = handle_head_adaptive_sync,
};

const zwlr_output_head_v1_listener* head_listener() { return &k_head_listener; }

void handle_manager_head(void*, zwlr_output_manager_v1*, zwlr_output_head_v1* h) {
    for (head_slot& s : g_slots) {
        if (s.live)
            continue;
        s = head_slot{};
        s.live = true;
        s.data.proxy = h;
        zwlr_output_head_v1_add_listener(h, head_listener(), &s);
        return;
    }
    // More heads than the pane holds (32 connectors is already an unusual machine): the
    // object is dropped rather than silently sharing a slot with a live head.
    zwlr_output_head_v1_destroy(h);
}

void handle_manager_done(void*, zwlr_output_manager_v1*, uint32_t serial) {
    g_serial = serial;
    dirty();
}

void handle_manager_finished(void*, zwlr_output_manager_v1*) {
    // A destructor event: libwayland frees the proxy as soon as this returns, so it must not
    // be destroyed here, and the heads went with it.
    if (g_inflight) {
        zwlr_output_configuration_v1_destroy(g_inflight);
        g_inflight = nullptr;
        g_inflight_name.clear();
    }
    for (head_slot& s : g_slots)
        if (s.live)
            clear_slot(s);
    g_available = false;
    g_manager = nullptr;
    g_serial = 0;
    dirty("The compositor stopped advertising output management.");
}

const zwlr_output_manager_v1_listener k_manager_listener = {
    .head = handle_manager_head,
    .done = handle_manager_done,
    .finished = handle_manager_finished,
};

void handle_registry_global(void*, wl_registry* reg, uint32_t name, const char* interface,
                            uint32_t version) {
    if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) != 0 || g_manager)
        return;
    g_manager_version = version < 4 ? version : 4;
    g_manager = static_cast<zwlr_output_manager_v1*>(
        wl_registry_bind(reg, name, &zwlr_output_manager_v1_interface, g_manager_version));
    zwlr_output_manager_v1_add_listener(g_manager, &k_manager_listener, nullptr);
    g_available = true;
    dirty();
}

void handle_registry_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener k_registry_listener = {
    .global = handle_registry_global,
    .global_remove = handle_registry_remove,
};

// ── the configuration's own events ──

/// `cfg` went (one way or another): the applied head keeps its staged values as its state
/// when it succeeded, and keeps them staged when it did not.
void settle_configuration(bool applied) {
    g_inflight = nullptr;
    if (applied)
        for (head_slot& s : g_slots)
            if (s.live && !g_inflight_name.empty() && s.data.name == g_inflight_name)
                s.data.staged = false;
    g_inflight_name.clear();
}

void handle_configuration_succeeded(void*, zwlr_output_configuration_v1* cfg) {
    zwlr_output_configuration_v1_destroy(cfg);
    settle_configuration(true);
    dirty("Applied — the compositor saved it to outputs.toml.");
}

void handle_configuration_failed(void*, zwlr_output_configuration_v1* cfg) {
    zwlr_output_configuration_v1_destroy(cfg);
    settle_configuration(false);
    dirty("The compositor refused the configuration — nothing changed.");
}

void handle_configuration_cancelled(void*, zwlr_output_configuration_v1* cfg) {
    // The heads changed under us (a hotplug): the configuration is void, and the staged
    // edits stay staged so they can be applied again against the new serial.
    zwlr_output_configuration_v1_destroy(cfg);
    settle_configuration(false);
    dirty("The configuration was cancelled (the outputs changed).");
}

const zwlr_output_configuration_v1_listener k_configuration_listener = {
    .succeeded = handle_configuration_succeeded,
    .failed = handle_configuration_failed,
    .cancelled = handle_configuration_cancelled,
};

} // namespace

export namespace settings {

bool outputs_available() { return g_available; }

const std::vector<display_head>& output_heads() { return g_view; }

std::string_view outputs_note() { return g_note; }

void stage_enabled(unsigned i, bool on) {
    head_slot* s = slot_at(i);
    if (!s)
        return;
    display_head& d = s->data;
    if (!d.staged) {
        // The first edit copies the live state, so an apply that touches one property
        // restates the rest as they are rather than as the pane's defaults.
        d.staged = true;
        d.staged_enabled = d.enabled;
        d.staged_scale = d.scale;
        d.staged_transform = d.transform;
        d.staged_x = d.x;
        d.staged_y = d.y;
        d.staged_adaptive = d.adaptive_sync;
        d.staged_mode = current_mode_index(*s);
    }
    d.staged_enabled = on;
    dirty();
}

void stage_mode(unsigned i, int mode_index) {
    head_slot* s = slot_at(i);
    if (!s || mode_slot_at(*s, mode_index) < 0)
        return;
    if (!s->data.staged)
        stage_enabled(i, s->data.enabled);
    s->data.staged_mode = mode_index;
    dirty();
}

void stage_scale(unsigned i, double scale) {
    head_slot* s = slot_at(i);
    if (!s)
        return;
    if (!s->data.staged)
        stage_enabled(i, s->data.enabled);
    s->data.staged_scale = std::clamp(scale, 0.25, 4.0);
    dirty();
}

void stage_transform(unsigned i, std::string_view transform) {
    head_slot* s = slot_at(i);
    if (!s)
        return;
    if (!s->data.staged)
        stage_enabled(i, s->data.enabled);
    s->data.staged_transform = std::string(transform);
    dirty();
}

void stage_position(unsigned i, int x, int y) {
    head_slot* s = slot_at(i);
    if (!s)
        return;
    if (!s->data.staged)
        stage_enabled(i, s->data.enabled);
    s->data.staged_x = x;
    s->data.staged_y = y;
    dirty();
}

void stage_adaptive(unsigned i, bool on) {
    head_slot* s = slot_at(i);
    if (!s)
        return;
    if (!s->data.staged)
        stage_enabled(i, s->data.enabled);
    s->data.staged_adaptive = on;
    dirty();
}

void discard_staged(unsigned i) {
    if (head_slot* s = slot_at(i)) {
        s->data.staged = false;
        dirty();
    }
}

void apply_head(unsigned i) {
    if (!g_manager || g_serial == 0) {
        dirty("The output manager is not ready.");
        return;
    }
    if (g_inflight) {
        dirty("An apply is already in flight.");
        return;
    }
    head_slot* target = slot_at(i);
    if (!target || !target->data.staged) {
        dirty("Nothing staged.");
        return;
    }
    // The compositor refuses a configuration that leaves every head disabled — there would
    // be no output to composite to — so say so here rather than spend a round trip on it.
    bool any_enabled = false;
    for (const head_slot& s : g_slots) {
        if (!s.live)
            continue;
        any_enabled = any_enabled || (&s == target ? s.data.staged_enabled : s.data.enabled);
    }
    if (!any_enabled) {
        dirty("At least one output has to stay enabled.");
        return;
    }
    zwlr_output_configuration_v1* cfg =
        zwlr_output_manager_v1_create_configuration(g_manager, g_serial);
    zwlr_output_configuration_v1_add_listener(cfg, &k_configuration_listener, nullptr);
    for (head_slot& s : g_slots) {
        if (!s.live)
            continue;
        const bool staged = &s == target;
        const display_head& d = s.data;
        auto* head = static_cast<zwlr_output_head_v1*>(d.proxy);
        if (!head)
            continue;
        if (!(staged ? d.staged_enabled : d.enabled)) {
            zwlr_output_configuration_v1_disable_head(cfg, head);
            continue;
        }
        zwlr_output_configuration_head_v1* hc =
            zwlr_output_configuration_v1_enable_head(cfg, head);
        // Restating a head's own state is what keeps its neighbours out of this edit: the
        // compositor reads an unchanged position as "still automatic" and an unchanged mode
        // as "keep following the preferred one" (ADR-008 H3), so the restatement changes
        // nothing. `set_mode` is omitted when there is no such mode to name (a disabled head
        // that was never given one).
        if (zwlr_output_mode_v1* m = mode_proxy(s, staged ? d.staged_mode : current_mode_index(s)))
            zwlr_output_configuration_head_v1_set_mode(hc, m);
        const double scale = staged ? d.staged_scale : d.scale;
        if (scale > 0.0)
            zwlr_output_configuration_head_v1_set_scale(hc, wl_fixed_from_double(scale));
        zwlr_output_configuration_head_v1_set_transform(
            hc, transform_index(staged && !d.staged_transform.empty() ? d.staged_transform
                                                                      : d.transform));
        zwlr_output_configuration_head_v1_set_position(
            hc, staged ? d.staged_x : d.x, staged ? d.staged_y : d.y);
        // Adaptive sync arrived in version 4; an older compositor has no such state.
        if (g_manager_version >= ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_SET_ADAPTIVE_SYNC_SINCE_VERSION)
            zwlr_output_configuration_head_v1_set_adaptive_sync(
                hc, (staged ? d.staged_adaptive : d.adaptive_sync)
                        ? ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED
                        : ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
    }
    zwlr_output_configuration_v1_apply(cfg);
    g_inflight = cfg;
    g_inflight_name = target->data.name;
    wl_display_flush(g_display);
    dirty("Applying…");
}

void start_outputs(void* native_display, void (*refresh)()) {
    g_refresh = refresh;
    g_display = static_cast<wl_display*>(native_display);
    if (!g_display)
        return;
    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &k_registry_listener, nullptr);
    wl_display_roundtrip(g_display); // the bind; the heads' state over the next dispatches
}

void stop_outputs() {
    if (g_inflight) {
        zwlr_output_configuration_v1_destroy(g_inflight);
        g_inflight = nullptr;
        g_inflight_name.clear();
    }
    for (head_slot& s : g_slots)
        if (s.live)
            clear_slot(s);
    if (g_manager) {
        // `stop` is the polite end: the compositor answers `finished`, which is where the
        // proxy is freed (a destructor event), so the object is left alone here.
        zwlr_output_manager_v1_stop(g_manager);
        g_manager = nullptr;
    }
    if (g_registry) {
        wl_registry_destroy(g_registry);
        g_registry = nullptr;
    }
    g_available = false;
    g_serial = 0;
    if (g_display)
        wl_display_flush(g_display);
}

void note_outputs_dirty() { dirty(); }

} // namespace settings

#else // !LUMEN_SETTINGS_HAS_OUTPUT_MANAGEMENT

export namespace settings {

bool outputs_available() { return false; }
const std::vector<display_head>& output_heads() {
    static const std::vector<display_head> none;
    return none;
}
std::string_view outputs_note() { return {}; }
void stage_enabled(unsigned, bool) {}
void stage_mode(unsigned, int) {}
void stage_scale(unsigned, double) {}
void stage_transform(unsigned, std::string_view) {}
void stage_position(unsigned, int, int) {}
void stage_adaptive(unsigned, bool) {}
void apply_head(unsigned) {}
void discard_staged(unsigned) {}
void start_outputs(void*, void (*)()) {}
void stop_outputs() {}
void note_outputs_dirty() {}

} // namespace settings

#endif
