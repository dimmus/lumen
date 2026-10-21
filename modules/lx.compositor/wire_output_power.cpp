module;

// zwlr_output_power_manager_v1 (wlr-output-power-management-unstable-v1.xml, v1): per-output
// power control. Part of the protocol wire split; shared declarations live in the :wire
// partition (wire.cppm), the state machine in :display_power (pure, tested on its own).
//
// Why this protocol (design I1): the compositor is the mechanism — it tracks activity, honours
// inhibitors, powers outputs off and back on, and wakes them on input — and the policy (when
// to blank) belongs to a client: lumen-shell by default, or swayidle + wlopm. GNOME
// (gsd-power → Mutter) and KDE (PowerDevil → KWin) keep timeouts out of the compositor too;
// sway, niri and Hyprland leave them to swayidle/hypridle. A public protocol serves both
// without a second, privileged path that would fight an external daemon.
//
// Any client may bind it (as in sway and niri; design X1 records it for the sandbox filter).
// Several objects per output are allowed — no exclusivity, as in wlroots: the last request
// wins and every object hears every `mode` change. `mode` is sent at once on creation and
// after each change the power state machine reports (off once the CRTC is down; on once the
// resume modeset went through). Without a display to power (headless, nested), or for a
// wl_output that stands for no output, the object is sent `failed` and goes inert.
//
// Protocol errors: `invalid_mode` for a mode outside {off, on} (checked before anything else:
// the request is malformed whatever the object's state).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>
#include <new>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_WLR_OUTPUT_POWER_MANAGEMENT_UNSTABLE_V1)
#include "wlr-output-power-management-unstable-v1-server-protocol.h"
#define LUMEN_HAS_OUTPUT_POWER 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :output;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

#if defined(LUMEN_HAS_OUTPUT_POWER)

/// One zwlr_output_power_v1. Inert (`live` false) after `failed`.
struct power_obj {
    wl_resource* resource = nullptr;
    /// The output slot its wl_output stood for at creation. The control belongs to the output,
    /// not to the wl_output object (wlroots): it survives wl_output.release and fails when
    /// that output goes.
    unsigned slot = 0;
    bool live = false;
    power_obj* prev = nullptr;
    power_obj* next = nullptr;
};

/// Newest first; allocated per request (an object outlives nothing it does not own).
power_obj* g_power_objects = nullptr;

void unlink(power_obj* o) {
    if (o->prev)
        o->prev->next = o->next;
    else
        g_power_objects = o->next;
    if (o->next)
        o->next->prev = o->prev;
    o->prev = o->next = nullptr;
}

power_obj* power_from(wl_resource* r) {
    auto* o = r ? static_cast<power_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return o && o->resource == r ? o : nullptr;
}

void power_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, "compositor.power", line);
}

void make_failed(power_obj* o) {
    if (!o->live)
        return;
    o->live = false;
    if (!client_dying(wl_resource_get_client(o->resource)))
        zwlr_output_power_v1_send_failed(o->resource);
}

void power_destroy_resource(wl_resource* r) {
    power_obj* o = power_from(r);
    if (!o)
        return;
    unlink(o);
    delete o;
}

[[nodiscard]] bool slot_live(unsigned slot) {
    return g_p0_ctx && g_p0_ctx->outputs && g_p0_ctx->outputs->slot_live(slot);
}

[[nodiscard]] bool power_available() {
    return g_p0_ctx && g_p0_ctx->set_output_power && g_p0_ctx->output_power_on;
}

void set_mode(struct wl_client*, struct wl_resource* r, uint32_t mode) {
    if (mode != ZWLR_OUTPUT_POWER_V1_MODE_OFF && mode != ZWLR_OUTPUT_POWER_V1_MODE_ON) {
        wl_resource_post_error(r, ZWLR_OUTPUT_POWER_V1_ERROR_INVALID_MODE,
                               "set_mode: %u is not a power mode (0 off, 1 on)", mode);
        return;
    }
    power_obj* o = power_from(r);
    if (!o || !o->live)
        return; // failed already: requests are ignored until the client destroys it
    const int slot = slot_live(o->slot) ? static_cast<int>(o->slot) : -1;
    if (slot < 0 || !power_available()) {
        make_failed(o);
        return;
    }
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        char line[96];
        std::snprintf(line, sizeof(line), "set_mode output=%d mode=%s client_object=%u", slot,
                      mode == ZWLR_OUTPUT_POWER_V1_MODE_ON ? "on" : "off",
                      wl_resource_get_id(r));
        power_log(lx::trace::level::debug, line);
    }
    g_p0_ctx->set_output_power(g_p0_ctx->idle_user, static_cast<unsigned>(slot),
                               mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

const struct zwlr_output_power_v1_interface power_impl = {
    .set_mode = &set_mode,
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void get_output_power(struct wl_client* client, struct wl_resource* manager, uint32_t id,
                      struct wl_resource* output) {
    wl_resource* r = wl_resource_create(client, &zwlr_output_power_v1_interface,
                                        wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* o = new (std::nothrow) power_obj{};
    if (!o) {
        wl_resource_set_implementation(r, &power_impl, nullptr, nullptr);
        wl_client_post_no_memory(client);
        return;
    }
    o->resource = r;
    o->next = g_power_objects;
    if (g_power_objects)
        g_power_objects->prev = o;
    g_power_objects = o;
    wl_resource_set_implementation(r, &power_impl, o, &power_destroy_resource);
    const int slot = output_resource_slot(output);
    o->slot = slot >= 0 ? static_cast<unsigned>(slot) : 0;
    if (slot < 0 || !power_available()) {
        o->live = true;
        make_failed(o); // no display to power, or a wl_output for no output
        return;
    }
    o->live = true;
    zwlr_output_power_v1_send_mode(r, g_p0_ctx->output_power_on(g_p0_ctx->idle_user,
                                                                 static_cast<unsigned>(slot))
                                          ? ZWLR_OUTPUT_POWER_V1_MODE_ON
                                          : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
}

const struct zwlr_output_power_manager_v1_interface power_manager_impl = {
    .get_output_power = &get_output_power,
    // Objects created from it remain valid ("until their appropriate destroy request").
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

#endif

} // namespace

#if defined(LUMEN_HAS_OUTPUT_POWER)
void bind_output_power_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &zwlr_output_power_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&power_manager_impl, g_p0_ctx, nullptr);
}
#endif

void output_power_outputs_changed() {
#if defined(LUMEN_HAS_OUTPUT_POWER)
    for (power_obj* o = g_power_objects; o; o = o->next)
        if (o->live && !slot_live(o->slot))
            make_failed(o);
#endif
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

void lx::compositor::notify_output_power(p0_protocol_context& ctx, unsigned slot, bool on) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_OUTPUT_POWER)
    for (p0::power_obj* o = p0::g_power_objects; o; o = o->next) {
        if (!o->live || p0::client_dying(wl_resource_get_client(o->resource)))
            continue;
        if (o->slot != slot)
            continue;
        zwlr_output_power_v1_send_mode(o->resource, on ? ZWLR_OUTPUT_POWER_V1_MODE_ON
                                                       : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
    }
#else
    (void)slot;
    (void)on;
#endif
}
