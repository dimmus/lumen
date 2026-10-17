module;

// wp_presentation (presentation-time.xml, stable, v2): per-commit presentation feedback.
// Part of the protocol wire split; shared declarations live in the :wire partition
// (wire.cppm), the bookkeeping in :presentation_feedback (pure, tested on its own).
//
// The presentation clock is CLOCK_MONOTONIC — what KMS flip timestamps are in
// (DRM_CAP_TIMESTAMP_MONOTONIC) and what steady_clock reads. A feedback object belongs to
// the content update of the wl_surface.commit that follows it; it moves with that update
// through the cached state of a synchronized subsurface and is queued when the update is
// applied. The scene's visibility pass samples it into the first published frame that shows
// the surface; the first content flip of a frame at least that new presents it, with the
// kernel's timestamp and vblank sequence (`vsync | hw_clock | hw_completion`; a blocking
// modeset's software-timed completion is `vsync` only). A newer update that reached the
// screen first, the surface's destruction or unmapping, or a display that stopped (VT switched
// away) discard it. `zero_copy` is never set: there is no direct scanout yet.
//
// Protocol errors: none can occur. `invalid_timestamp` and `invalid_flag` belong to requests
// removed before the protocol was declared stable; no v1 or v2 request can raise them.
//
// Feedback objects have no requests; `presented` and `discarded` are destructor events — the
// resource is destroyed right after either is sent.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>
#include <ctime>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if __has_include("presentation-time-server-protocol.h")
#include "presentation-time-server-protocol.h"
#define LUMEN_HAS_PRESENTATION 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :surface;
import :output;
import :presentation_feedback;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

lx::compositor::presentation_feedback_table* table() {
    return g_p0_ctx ? g_p0_ctx->presentation : nullptr;
}

[[nodiscard]] unsigned long long monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<unsigned long long>(ts.tv_nsec);
}

/// Events sent since the last per-flip debug line (debug logging only).
struct counters {
    unsigned presented = 0;
    unsigned discarded = 0;
};
counters g_counts{};

/// The output whose flip is presenting feedback right now (set around `on_flip`): the
/// `sync_output` of what it presents.
int g_flip_output = -1;

#if defined(LUMEN_HAS_PRESENTATION)

void send_sync_output(wl_resource* output, void* feedback) {
    wp_presentation_feedback_send_sync_output(static_cast<wl_resource*>(feedback), output);
}

void sink_presented(void*, void* resource, lx::surface_id, const present_info& info) {
    auto* r = static_cast<wl_resource*>(resource);
    wl_client* client = wl_resource_get_client(r);
    // A client being torn down: libwayland destroys the object itself (its destructor then
    // finds the slot already free).
    if (client_dying(client))
        return;
    // sync_output: the output whose flip presented it — the surface's pace output (M8).
    if (g_flip_output >= 0)
        for_each_output_resource(client, static_cast<unsigned>(g_flip_output), &send_sync_output, r);
    const unsigned long long sec = info.timestamp_ns / 1'000'000'000ull;
    const auto nsec = static_cast<std::uint32_t>(info.timestamp_ns % 1'000'000'000ull);
    wp_presentation_feedback_send_presented(
        r, static_cast<std::uint32_t>(sec >> 32), static_cast<std::uint32_t>(sec), nsec,
        info.refresh_ns, static_cast<std::uint32_t>(info.seq >> 32),
        static_cast<std::uint32_t>(info.seq), info.flags);
    wl_resource_destroy(r);
    ++g_counts.presented;
}

void sink_discarded(void*, void* resource) {
    auto* r = static_cast<wl_resource*>(resource);
    if (client_dying(wl_resource_get_client(r)))
        return;
    wp_presentation_feedback_send_discarded(r);
    wl_resource_destroy(r);
    ++g_counts.discarded;
}

#endif

[[nodiscard]] lx::compositor::feedback_sink sink() {
    lx::compositor::feedback_sink k{};
#if defined(LUMEN_HAS_PRESENTATION)
    k.presented = &sink_presented;
    k.discarded = &sink_discarded;
#endif
    return k;
}

#if defined(LUMEN_HAS_PRESENTATION)

/// The feedback object's destructor: a client that disconnects with feedback in flight.
/// The user data is the table handle; the table ignores it once the slot is no longer this
/// resource's (it was presented or discarded, which destroys the resource from here).
void feedback_destroy_resource(wl_resource* r) {
    auto* t = table();
    const auto handle = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(
        wl_resource_get_user_data(r)));
    if (t && handle)
        t->forget_resource(handle, r);
}

/// One `warn` a minute at most when feedback is refused for lack of room (never silent).
unsigned long long g_overflow_warned_ns = 0;

void presentation_feedback_request(struct wl_client* client, struct wl_resource* resource,
                                   struct wl_resource* surface_res, uint32_t id) {
    wl_resource* fb = wl_resource_create(client, &wp_presentation_feedback_interface,
                                         wl_resource_get_version(resource), id);
    if (!fb) {
        wl_client_post_no_memory(client);
        return;
    }
    // No requests: the object only ever receives its one destructor event.
    wl_resource_set_implementation(fb, nullptr, nullptr, &feedback_destroy_resource);
    surface_obj* s = surface_from(surface_res);
    auto* t = table();
    const unsigned handle = s && t ? t->create(fb, s->id, s->pending.feedback) : 0;
    if (!handle) {
        if (s && t) {
            const unsigned long long now = monotonic_ns();
            if (g_overflow_warned_ns == 0 || now - g_overflow_warned_ns >= 60'000'000'000ull) {
                g_overflow_warned_ns = now;
                lx::trace::logger::global().log(
                    lx::trace::level::warn, "compositor.presentation",
                    "presentation feedback table full — new feedback is discarded at once");
            }
        }
        wp_presentation_feedback_send_discarded(fb);
        wl_resource_destroy(fb);
        return;
    }
    wl_resource_set_user_data(fb, reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)));
}

const struct wp_presentation_interface presentation_impl = {
    // Feedback objects created from it live on ("Existing objects created by this object
    // are not affected").
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .feedback = &presentation_feedback_request,
};

#endif

} // namespace

#if defined(LUMEN_HAS_PRESENTATION)

void bind_presentation(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_presentation_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&presentation_impl, g_p0_ctx, nullptr);
    if (auto* native = static_cast<wl_resource*>(res.native()))
        wp_presentation_send_clock_id(native, CLOCK_MONOTONIC);
}

#endif

void presentation_merge(surface_obj* s) {
    if (auto* t = table())
        t->on_merge(s->pending.feedback, s->cached.feedback, sink());
}

void presentation_apply(surface_obj* s) {
    if (auto* t = table(); t && g_p0_ctx)
        t->on_apply(s->id, s->cached.feedback, g_p0_ctx->building_frame, sink());
}

void presentation_surface_destroyed(surface_obj* s) {
    auto* t = table();
    if (!t)
        return;
    const lx::compositor::feedback_sink k = sink();
    t->discard_list(s->pending.feedback, k);
    t->discard_list(s->cached.feedback, k);
    t->on_surface_gone(s->id, k);
}

void presentation_surface_unmapped(lx::surface_id surface) {
    if (auto* t = table())
        t->on_surface_gone(surface, sink());
}

void presentation_drop_cached(surface_obj* s) {
    if (auto* t = table())
        t->discard_list(s->cached.feedback, sink());
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

void lx::compositor::presentation_published(p0_protocol_context& ctx, unsigned frame,
                                            unsigned slot) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!ctx.presentation)
        return;
    ctx.presentation->on_publish(
        frame, slot,
        [](void*, lx::surface_id surface, unsigned f, unsigned output) {
            return p0::surface_visible(surface, f, output);
        },
        [](void*, lx::surface_id surface) { return p0::surface_pace_output(surface); }, nullptr);
#else
    (void)ctx;
    (void)frame;
    (void)slot;
#endif
}

void lx::compositor::presentation_flipped(p0_protocol_context& ctx, unsigned frame, unsigned slot,
                                          const present_info& info) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!ctx.presentation)
        return;
    p0::g_flip_output = static_cast<int>(slot);
    ctx.presentation->on_flip(frame, slot, info, p0::sink());
    p0::g_flip_output = -1;
    if (p0::g_counts.presented + p0::g_counts.discarded == 0)
        return;
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "flip frame=%u t=%llu.%09llu seq=%llu refresh_ns=%u flags=0x%x "
                      "presented=%u discarded=%u output=%u",
                      frame, info.timestamp_ns / 1'000'000'000ull,
                      info.timestamp_ns % 1'000'000'000ull, info.seq, info.refresh_ns, info.flags,
                      p0::g_counts.presented, p0::g_counts.discarded, slot);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.presentation", line);
    }
    p0::g_counts = {};
#else
    (void)ctx;
    (void)frame;
    (void)slot;
    (void)info;
#endif
}

void lx::compositor::presentation_discard_output(p0_protocol_context& ctx, unsigned slot) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!ctx.presentation)
        return;
    ctx.presentation->discard_output(
        slot, [](void*, lx::surface_id surface) { return p0::surface_pace_output(surface); },
        nullptr, p0::sink());
#else
    (void)ctx;
    (void)slot;
#endif
}

void lx::compositor::presentation_discard(p0_protocol_context& ctx, bool queued_only) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!ctx.presentation)
        return;
    if (queued_only)
        ctx.presentation->discard_queued(p0::sink());
    else
        ctx.presentation->discard_unpresented(p0::sink());
#else
    (void)ctx;
    (void)queued_only;
#endif
}
