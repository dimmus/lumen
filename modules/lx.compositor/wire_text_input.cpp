module;

// zwp_text_input_manager_v3 (text-input-unstable-v3.xml, v2): text inputs and the events the
// relay sends them. Part of the protocol wire split; shared declarations live in the :wire
// partition (wire.cppm), the rules in :text_input (pure, tested on its own), the input method
// side in wire_input_method.cpp.
//
// Every error of the protocol:
//   zwp_text_input_v3.invalid_action   set_available_actions (v2) with `none`, an action twice,
//                                      or a value that is not an action
// set_surrounding_text over 4000 bytes is not an error in the protocol: the text is cut at
// the last UTF-8 boundary within 4000 bytes (one debug line per text input).
// show_input_panel / hide_input_panel (v2) are hints: there is no on-screen keyboard to show.
// `action`, `language` and `preedit_hint` (v2 events) are never sent — input-method-v2 has no
// way for an input method to produce them.
//
// Objects are allocated per get_text_input (client-driven, never per frame; 8 KiB each for
// the two state copies) and freed with their resource. The wl_seat argument names Lumen's one
// seat whichever wl_seat object it is.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_TEXT_INPUT_UNSTABLE_V3)
#include "text-input-unstable-v3-server-protocol.h"
#define LUMEN_HAS_TEXT_INPUT 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :surface;
import :text_input;
import :wire;

namespace {

constexpr lx::trace::category log_ime{"compositor.ime"};

} // namespace

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_TEXT_INPUT)

namespace {

[[nodiscard]] lx::compositor::text_input_relay* relay() {
    return g_p0_ctx ? g_p0_ctx->text_input : nullptr;
}

/// The wire record: the relay's record and a per-object flag for the truncation log line.
struct text_input_obj {
    lx::compositor::text_input_record record{};
    bool truncation_logged = false;
};

text_input_obj* ti_from(wl_resource* r) {
    return r ? static_cast<text_input_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

void ti_destroy_resource(wl_resource* r) {
    text_input_obj* o = ti_from(r);
    if (!o)
        return;
    if (auto* rl = relay())
        rl->remove(o->record);
    delete o;
}

/// Bits of the zwp_text_input_v3.action values an array names; false (with the reason) when
/// it holds `none`, an action twice or a value outside the enum.
[[nodiscard]] bool parse_actions(const wl_array* array, unsigned& mask) {
    mask = 0;
    if (!array || array->size % sizeof(uint32_t) != 0)
        return false;
    const auto* v = static_cast<const uint32_t*>(array->data);
    const std::size_t n = array->size / sizeof(uint32_t);
    for (std::size_t i = 0; i < n; ++i) {
        const uint32_t a = v[i];
        // `none` is not an action one can offer; only `submit` (1) exists beyond it.
        if (a == ZWP_TEXT_INPUT_V3_ACTION_NONE || a > ZWP_TEXT_INPUT_V3_ACTION_SUBMIT)
            return false;
        const unsigned bit = 1u << a;
        if (mask & bit)
            return false;
        mask |= bit;
    }
    return true;
}

const struct zwp_text_input_v3_interface ti_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .enable =
        [](struct wl_client*, struct wl_resource* r) {
            if (auto* o = ti_from(r); o && relay())
                relay()->enable(o->record);
        },
    .disable =
        [](struct wl_client*, struct wl_resource* r) {
            if (auto* o = ti_from(r); o && relay())
                relay()->disable(o->record);
        },
    .set_surrounding_text =
        [](struct wl_client*, struct wl_resource* r, const char* text, int32_t cursor,
           int32_t anchor) {
            auto* o = ti_from(r);
            if (!o || !relay())
                return;
            const std::string_view s = text ? text : "";
            if (s.size() > lx::compositor::k_ime_text_max && !o->truncation_logged &&
                log_ime.enabled(lx::trace::level::debug)) {
                o->truncation_logged = true;
                log_ime.debug("text input surrounding text of {} bytes cut to the protocol's 4000",
                              s.size());
            }
            relay()->set_surrounding(o->record, s, cursor, anchor);
        },
    .set_text_change_cause =
        [](struct wl_client*, struct wl_resource* r, uint32_t cause) {
            if (auto* o = ti_from(r); o && relay())
                relay()->set_change_cause(o->record, cause);
        },
    .set_content_type =
        [](struct wl_client*, struct wl_resource* r, uint32_t hint, uint32_t purpose) {
            if (auto* o = ti_from(r); o && relay())
                relay()->set_content_type(o->record, hint, purpose);
        },
    .set_cursor_rectangle =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
            if (auto* o = ti_from(r); o && relay())
                relay()->set_cursor_rect(o->record, {x, y, w, h});
        },
    .commit =
        [](struct wl_client*, struct wl_resource* r) {
            if (auto* o = ti_from(r); o && relay())
                relay()->commit(o->record);
        },
    .set_available_actions =
        [](struct wl_client*, struct wl_resource* r, struct wl_array* actions) {
            unsigned mask = 0;
            if (!parse_actions(actions, mask)) {
                wl_resource_post_error(r, ZWP_TEXT_INPUT_V3_ERROR_INVALID_ACTION,
                                       "available actions hold none, a duplicate, or a value "
                                       "that is not an action");
                return;
            }
            if (auto* o = ti_from(r); o && relay())
                relay()->set_actions(o->record, mask);
        },
    // Hints only: there is no on-screen keyboard.
    .show_input_panel = [](struct wl_client*, struct wl_resource*) {},
    .hide_input_panel = [](struct wl_client*, struct wl_resource*) {},
};

void manager_get_text_input(struct wl_client* client, struct wl_resource* manager, uint32_t id,
                            struct wl_resource*) {
    const int version = wl_resource_get_version(manager);
    wl_resource* res = wl_resource_create(client, &zwp_text_input_v3_interface, version, id);
    auto* o = res ? new (std::nothrow) text_input_obj{} : nullptr;
    if (!o) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    o->record.resource = res;
    o->record.client = client;
    o->record.version = static_cast<unsigned>(version);
    wl_resource_set_implementation(res, &ti_impl, o, &ti_destroy_resource);
    if (auto* rl = relay())
        rl->add(o->record);
}

const struct zwp_text_input_manager_v3_interface manager_impl = {
    // Text inputs created from it live on.
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_text_input = &manager_get_text_input,
};

// ── the relay's zwp_text_input_v3 events ─────────────────────────────────────

[[nodiscard]] bool can_send(void* ti) {
    return ti && !client_dying(wl_resource_get_client(static_cast<wl_resource*>(ti)));
}

void ev_enter(void*, void* ti, void* surface) {
    if (!can_send(ti) || !surface)
        return;
    zwp_text_input_v3_send_enter(static_cast<wl_resource*>(ti), static_cast<wl_resource*>(surface));
    if (log_ime.enabled(lx::trace::level::debug)) {
        log_ime.debug("text input enter ti={} surface={}",
                      wl_resource_get_id(static_cast<wl_resource*>(ti)),
                      wl_resource_get_id(static_cast<wl_resource*>(surface)));
    }
}

void ev_leave(void*, void* ti, void* surface) {
    if (!can_send(ti) || !surface)
        return;
    zwp_text_input_v3_send_leave(static_cast<wl_resource*>(ti), static_cast<wl_resource*>(surface));
    if (log_ime.enabled(lx::trace::level::debug)) {
        log_ime.debug("text input leave ti={}", wl_resource_get_id(static_cast<wl_resource*>(ti)));
    }
}

void ev_preedit(void*, void* ti, const char* text, int begin, int end) {
    if (can_send(ti))
        zwp_text_input_v3_send_preedit_string(static_cast<wl_resource*>(ti), text, begin, end);
}

void ev_commit_string(void*, void* ti, const char* text) {
    if (can_send(ti))
        zwp_text_input_v3_send_commit_string(static_cast<wl_resource*>(ti), text);
}

void ev_delete(void*, void* ti, unsigned before, unsigned after) {
    if (can_send(ti))
        zwp_text_input_v3_send_delete_surrounding_text(static_cast<wl_resource*>(ti), before,
                                                       after);
}

void ev_done(void*, void* ti, unsigned serial) {
    if (can_send(ti))
        zwp_text_input_v3_send_done(static_cast<wl_resource*>(ti), serial);
}

} // namespace

void bind_text_input_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_text_input_manager_v3_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

void text_input_sink_events(lx::compositor::text_input_sink& sink) {
    sink.enter = &ev_enter;
    sink.leave = &ev_leave;
    sink.preedit = &ev_preedit;
    sink.commit_string = &ev_commit_string;
    sink.delete_surrounding = &ev_delete;
    sink.done = &ev_done;
}

#else // !LUMEN_HAS_TEXT_INPUT

void text_input_sink_events(lx::compositor::text_input_sink&) {}

#endif // LUMEN_HAS_TEXT_INPUT

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
