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

namespace {

using namespace settings;

std::vector<display_head> g_heads;            // NOLINT(cppcoreguidelines-avoid-non-const-global-variable)
bool g_available = false;                     // NOLINT
std::string g_note;                           // NOLINT
wl_display* g_display = nullptr;              // NOLINT
wl_registry* g_registry = nullptr;            // NOLINT
zwlr_output_manager_v1* g_manager = nullptr;  // NOLINT
uint32_t g_serial = 0;                        // the last done
zwlr_output_configuration_v1* g_inflight = nullptr; // one apply at a time
unsigned g_inflight_head = ~0u;
void (*g_refresh)() = nullptr;                // NOLINT

void dirty(const char* note = nullptr) {
    if (note)
        g_note = note;
    if (g_refresh)
        g_refresh();
}

[[nodiscard]] display_head* head_of_proxy(void* proxy) {
    for (display_head& d : g_heads)
        if (d.proxy == proxy)
            return &d;
    return nullptr;
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
    auto* m = static_cast<display_head::mode_info*>(data);
    for (display_head& d : g_heads)
        std::erase_if(d.modes, [m](const display_head::mode_info& x) { return &x == m; });
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
    static_cast<display_head*>(data)->name = name;
    dirty();
}

void handle_head_description(void* data, zwlr_output_head_v1*, const char* d) {
    static_cast<display_head*>(data)->description = d;
    dirty();
}

void handle_head_enabled(void* data, zwlr_output_head_v1*, int32_t on) {
    static_cast<display_head*>(data)->enabled = on != 0;
    dirty();
}

void handle_head_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    auto* head = static_cast<display_head*>(data);
    head->modes.emplace_back();
    head->modes.back().proxy = mode;
    zwlr_output_mode_v1_add_listener(mode, mode_listener(), &head->modes.back());
}

void handle_head_scale(void* data, zwlr_output_head_v1*, wl_fixed_t f) {
    static_cast<display_head*>(data)->scale = wl_fixed_to_double(f);
    dirty();
}

void handle_head_transform(void* data, zwlr_output_head_v1*, int32_t t) {
    static constexpr const char* k_names[] = {"normal",   "90",          "180",
                                              "270",     "flipped",     "flipped-90",
                                              "flipped-180", "flipped-270"};
    auto* head = static_cast<display_head*>(data);
    head->transform = t >= 0 && t < 8 ? k_names[t] : "normal";
    dirty();
}

void handle_head_position(void* data, zwlr_output_head_v1*, int32_t x, int32_t y) {
    auto* head = static_cast<display_head*>(data);
    head->x = x;
    head->y = y;
    dirty();
}

void handle_head_current_mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    auto* head = static_cast<display_head*>(data);
    for (display_head::mode_info& m : head->modes) {
        m.current = m.proxy == mode;
        if (m.current) {
            head->w = m.w;
            head->h = m.h;
            head->refresh_mhz = m.refresh_mhz;
        }
    }
    dirty();
}

void handle_head_adaptive_sync(void* data, zwlr_output_head_v1*, uint32_t state) {
    static_cast<display_head*>(data)->adaptive_sync = state == 1; // enabled
    dirty();
}

void handle_head_finished(void* data, zwlr_output_head_v1* head) {
    auto* d = static_cast<display_head*>(data);
    if (d->proxy)
        zwlr_output_head_v1_destroy(head);
    std::erase_if(g_heads, [d](const display_head& x) { return &x == d; });
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
    display_head& d = g_heads.emplace_back();
    d.proxy = h;
    zwlr_output_head_v1_add_listener(h, head_listener(), &d);
}

void handle_manager_done(void*, zwlr_output_manager_v1*, uint32_t serial) {
    g_serial = serial;
    dirty();
}

void handle_manager_finished(void*, zwlr_output_manager_v1*) {
    g_available = false;
    g_heads.clear();
    g_manager = nullptr;
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
    const uint32_t v = version < 4 ? version : 4;
    g_manager = static_cast<zwlr_output_manager_v1*>(
        wl_registry_bind(reg, name, &zwlr_output_manager_v1_interface, v));
    zwlr_output_manager_v1_add_listener(g_manager, &k_manager_listener, nullptr);
    g_available = true;
}

void handle_registry_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener k_registry_listener = {
    .global = handle_registry_global,
    .global_remove = handle_registry_remove,
};

// ── the configuration's own events ──

void handle_configuration_succeeded(void*, zwlr_output_configuration_v1* cfg) {
    zwlr_output_configuration_v1_destroy(cfg);
    if (g_inflight == cfg) {
        g_inflight = nullptr;
        if (g_inflight_head < g_heads.size())
            g_heads[g_inflight_head].staged = false;
        g_inflight_head = ~0u;
    }
    dirty("Applied — the compositor saved it to outputs.toml.");
}

void handle_configuration_failed(void*, zwlr_output_configuration_v1* cfg) {
    zwlr_output_configuration_v1_destroy(cfg);
    if (g_inflight == cfg) {
        g_inflight = nullptr;
        g_inflight_head = ~0u;
    }
    dirty("The compositor refused the configuration — nothing changed.");
}

void handle_configuration_cancelled(void*, zwlr_output_configuration_v1* cfg) {
    // The heads changed under us (a hotplug); the configuration is void.
    zwlr_output_configuration_v1_destroy(cfg);
    if (g_inflight == cfg) {
        g_inflight = nullptr;
        g_inflight_head = ~0u;
    }
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

const std::vector<display_head>& output_heads() { return g_heads; }

std::string_view outputs_note() { return g_note; }

void stage_enabled(unsigned i, bool on) {
    if (i >= g_heads.size())
        return;
    display_head& d = g_heads[i];
    if (!d.staged) {
        d.staged = true;
        d.staged_enabled = d.enabled;
        d.staged_scale = d.scale;
        d.staged_transform = d.transform;
        d.staged_x = d.x;
        d.staged_y = d.y;
        d.staged_adaptive = d.adaptive_sync;
        d.staged_mode = -1;
        for (unsigned m = 0; m < d.modes.size(); ++m)
            if (d.modes[m].current)
                d.staged_mode = static_cast<int>(m);
    }
    d.staged_enabled = on;
    dirty();
}

void stage_mode(unsigned i, int mode_index) {
    if (i >= g_heads.size())
        return;
    display_head& d = g_heads[i];
    if (!d.staged) {
        stage_enabled(i, d.enabled); // stages everything, then we refine
        d.staged_enabled = d.enabled;
    }
    d.staged_mode = mode_index;
    dirty();
}

void stage_scale(unsigned i, double scale) {
    if (i >= g_heads.size())
        return;
    display_head& d = g_heads[i];
    if (!d.staged)
        stage_enabled(i, d.enabled);
    d.staged_enabled = d.enabled;
    d.staged_scale = std::clamp(scale, 0.25, 4.0);
    dirty();
}

void stage_transform(unsigned i, std::string_view transform) {
    if (i >= g_heads.size())
        return;
    display_head& d = g_heads[i];
    if (!d.staged)
        stage_enabled(i, d.enabled);
    d.staged_enabled = d.enabled;
    d.staged_transform = std::string(transform);
    dirty();
}

void stage_position(unsigned i, int x, int y) {
    if (i >= g_heads.size())
        return;
    display_head& d = g_heads[i];
    if (!d.staged)
        stage_enabled(i, d.enabled);
    d.staged_enabled = d.enabled;
    d.staged_x = x;
    d.staged_y = y;
    dirty();
}

void stage_adaptive(unsigned i, bool on) {
    if (i >= g_heads.size())
        return;
    display_head& d = g_heads[i];
    if (!d.staged)
        stage_enabled(i, d.enabled);
    d.staged_enabled = d.enabled;
    d.staged_adaptive = on;
    dirty();
}

void discard_staged(unsigned i) {
    if (i >= g_heads.size())
        return;
    g_heads[i].staged = false;
    dirty();
}

void apply_head(unsigned i) {
    if (!g_manager || g_serial == 0 || i >= g_heads.size()) {
        dirty("The output manager is not ready.");
        return;
    }
    if (g_inflight) {
        dirty("An apply is already in flight.");
        return;
    }
    display_head& d = g_heads[i];
    if (!d.staged) {
        dirty("Nothing staged.");
        return;
    }
    zwlr_output_configuration_v1* cfg =
        zwlr_output_manager_v1_create_configuration(g_manager, g_serial);
    zwlr_output_configuration_v1_add_listener(cfg, &k_configuration_listener, nullptr);
    if (d.staged_enabled) {
        zwlr_output_configuration_head_v1* hc = zwlr_output_configuration_v1_enable_head(
            cfg, static_cast<zwlr_output_head_v1*>(d.proxy));
        if (d.staged_mode >= 0 &&
            d.staged_mode < static_cast<int>(d.modes.size()) && d.modes[d.staged_mode].proxy)
            zwlr_output_configuration_head_v1_set_mode(
                hc, static_cast<zwlr_output_mode_v1*>(d.modes[d.staged_mode].proxy));
        zwlr_output_configuration_head_v1_set_scale(
            hc, wl_fixed_from_double(d.staged_scale));
        zwlr_output_configuration_head_v1_set_transform(
            hc, transform_index(d.staged_transform.empty() ? d.transform
                                                           : d.staged_transform));
        zwlr_output_configuration_head_v1_set_position(hc, d.staged_x, d.staged_y);
        zwlr_output_configuration_head_v1_set_adaptive_sync(
            hc, d.staged_adaptive ? ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED
                                  : ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
    } else {
        zwlr_output_configuration_v1_disable_head(cfg,
                                                  static_cast<zwlr_output_head_v1*>(d.proxy));
    }
    zwlr_output_configuration_v1_apply(cfg);
    g_inflight = cfg;
    g_inflight_head = i;
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
    if (g_registry)
        wl_registry_destroy(g_registry);
    g_registry = nullptr;
    g_manager = nullptr;
    g_available = false;
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
