module;

// ext_image_copy_capture_cursor_session_v1: the pointer of a capture source as its own image
// plus metadata (design-app-compat-capture D.7, CC4) — what a remote desktop (wayvnc) draws
// on its side instead of having the pointer painted into every frame.
//
// `enter` when the pointer image starts to intersect the source's area (before `position` and
// `hotspot`), `leave` when it stops; `position` is the hotspot in the source buffer's pixels
// (may be negative or beyond the buffer), `hotspot` the offset inside the image, sent with the
// first frame after it changed (it takes effect at the next `ready`). At most one update per
// tick (the outputs' refresh). The capture session made by `get_capture_session` produces
// frames of the image itself: constraints `buffer_size` = the image at the source's scale and
// `shm_format` ARGB8888; the first frame at once, later ones when the image changes.
//
// Errors: `duplicate_session` (get_capture_session twice); the frame's are the ones of every
// capture frame (`no_buffer`, `invalid_buffer_damage`, `already_captured`). While the session
// is locked the pointer is nobody's business: `leave`, and frames wait for the unlock.
//
// UI only (the image is the cursor manager's, a few KiB copied under the shm access guard).
// Bounded: 16 cursor sessions.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_EXT_IMAGE_CAPTURE_SOURCE_V1) &&                                    \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_COPY_CAPTURE_V1)
#include "ext-image-copy-capture-v1-server-protocol.h"
#define LUMEN_HAS_IMAGE_CAPTURE 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import :toplevel;
import :wm;
import :decoration;
import :session_lock;
import :cursor;
import :xcursor;
import :capture;
import :capture_engine;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_IMAGE_CAPTURE)

namespace {

constexpr unsigned k_cursor_sessions = 16;

struct cursor_frame_obj;
struct cursor_capture_obj;

/// ext_image_copy_capture_cursor_session_v1. Allocated per request; linked into a fixed
/// table (`g_cursors`) while it lives.
struct cursor_obj {
    wl_resource* resource = nullptr;
    capture_source_kind kind = capture_source_kind::none;
    unsigned slot = 0;
    lx::output_id output{};
    lx::toplevel_id toplevel{};
    char identifier[33]{};
    cursor_capture_obj* capture = nullptr;
    bool capture_made = false;
    bool entered = false;
    lx::point2i position{};
    bool position_sent = false;
    lx::point2i hotspot{};
    bool hotspot_sent = false;
    bool stopped = false;
};

/// The capture session of a cursor session (an ext_image_copy_capture_session_v1).
struct cursor_capture_obj {
    wl_resource* resource = nullptr;
    cursor_obj* cursor = nullptr;
    cursor_frame_obj* frame = nullptr;
    lx::size2i size{};
    bool constraints_sent = false;
    bool stopped = false;
    /// The image generation of the last frame delivered (0: none yet).
    unsigned delivered_generation = 0;
};

struct cursor_frame_obj {
    wl_resource* resource = nullptr;
    cursor_capture_obj* session = nullptr;
    capture_frame_requests req{};
    wl_resource* buffer = nullptr;
    wl_listener listener{};
    bool requested = false;
    bool done = false;
};

cursor_obj* g_cursors[k_cursor_sessions]{};

[[nodiscard]] unsigned long long mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<unsigned long long>(ts.tv_nsec);
}

/// Where the source puts the pointer, in its buffer's pixels: the scale the image is taken
/// at, the hotspot's pixel and the image's rect. False when the source is gone.
struct placement {
    double scale = 1.0;
    lx::point2i hot_pixel{};
    lx::rect2i area{};
};

bool place(const cursor_obj& c, double x, double y, placement& out) {
    const capture_host* h = g_p0_ctx ? g_p0_ctx->capture : nullptr;
    if (!h)
        return false;
    if (c.kind == capture_source_kind::output) {
        const capture_output_info& o = h->outputs[c.slot % 16];
        if (!o.live || o.id != c.output)
            return false;
        out.scale = o.scale;
        const lx::rect2i r = capture_cursor_rect(x, y, o.origin, o.scale, o.transform, o.mode, 1,
                                                 1, 0, 0);
        out.hot_pixel = {r.x, r.y};
        out.area = {0, 0, o.mode.width, o.mode.height};
        return true;
    }
    if (c.kind == capture_source_kind::toplevel && g_p0_ctx->toplevels) {
        const toplevel_record* rec = g_p0_ctx->toplevels->find(c.toplevel);
        if (!rec || !rec->mapped || std::strcmp(rec->foreign_identifier, c.identifier) != 0)
            return false;
        const lx::rect2i f = frame_rect(rec->geometry, window_manager::extents_of(*rec));
        const std::uint32_t s120 = expected_scale_120(surface_by_id(rec->surface));
        out.scale = s120 ? s120 / 120.0 : 1.0;
        out.hot_pixel = {static_cast<int>(std::floor((x - f.x) * out.scale)),
                         static_cast<int>(std::floor((y - f.y) * out.scale))};
        out.area = {0, 0, static_cast<int>(f.width * out.scale + 0.5),
                    static_cast<int>(f.height * out.scale + 0.5)};
        return true;
    }
    return false;
}

void frame_unwatch(cursor_frame_obj* f) {
    if (f->buffer) {
        wl_list_remove(&f->listener.link);
        wl_list_init(&f->listener.link);
    }
    f->buffer = nullptr;
}

void frame_fail(cursor_frame_obj* f, capture_failure reason) {
    if (!f || f->done)
        return;
    f->done = true;
    ext_image_copy_capture_frame_v1_send_failed(f->resource, static_cast<std::uint32_t>(reason));
}

void capture_stop(cursor_capture_obj* cs) {
    if (!cs || cs->stopped)
        return;
    cs->stopped = true;
    if (cs->frame && cs->frame->requested)
        frame_fail(cs->frame, capture_failure::stopped);
    ext_image_copy_capture_session_v1_send_stopped(cs->resource);
}

void cursor_stop(cursor_obj* c) {
    if (!c || c->stopped)
        return;
    c->stopped = true;
    if (c->entered) {
        ext_image_copy_capture_cursor_session_v1_send_leave(c->resource);
        c->entered = false;
    }
    capture_stop(c->capture);
}

/// Copies the pointer image into the frame's buffer and sends ready (the hotspot first, when
/// it changed: it takes effect with this ready).
void deliver(cursor_capture_obj* cs, const cursor_image_view& img, unsigned generation) {
    cursor_frame_obj* f = cs->frame;
    if (!f || !f->requested || f->done)
        return;
    wl_shm_buffer* shm = f->buffer ? wl_shm_buffer_get(f->buffer) : nullptr;
    if (!shm || wl_shm_buffer_get_format(shm) != WL_SHM_FORMAT_ARGB8888 ||
        wl_shm_buffer_get_width(shm) != cs->size.width ||
        wl_shm_buffer_get_height(shm) != cs->size.height ||
        static_cast<unsigned>(wl_shm_buffer_get_stride(shm)) < img.width * 4u) {
        frame_fail(f, capture_failure::buffer_constraints);
        return;
    }
    const unsigned stride = static_cast<unsigned>(wl_shm_buffer_get_stride(shm));
    wl_shm_buffer_begin_access(shm);
    auto* dst = static_cast<unsigned char*>(wl_shm_buffer_get_data(shm));
    for (unsigned y = 0; y < img.height; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * stride,
                    img.pixels + static_cast<std::size_t>(y) * img.stride_px, img.width * 4u);
    wl_shm_buffer_end_access(shm);
    cursor_obj* c = cs->cursor;
    if (c && c->entered && (!c->hotspot_sent || c->hotspot.x != img.hot_x ||
                            c->hotspot.y != img.hot_y)) {
        c->hotspot = {img.hot_x, img.hot_y};
        c->hotspot_sent = true;
        ext_image_copy_capture_cursor_session_v1_send_hotspot(c->resource, img.hot_x, img.hot_y);
    }
    ext_image_copy_capture_frame_v1_send_transform(f->resource, 0);
    ext_image_copy_capture_frame_v1_send_damage(f->resource, 0, 0, cs->size.width, cs->size.height);
    const unsigned long long now = mono_ns();
    const unsigned long long sec = now / 1'000'000'000ull;
    ext_image_copy_capture_frame_v1_send_presentation_time(
        f->resource, static_cast<std::uint32_t>(sec >> 32),
        static_cast<std::uint32_t>(sec & 0xFFFFFFFFull),
        static_cast<std::uint32_t>(now % 1'000'000'000ull));
    ext_image_copy_capture_frame_v1_send_ready(f->resource);
    f->done = true;
    cs->delivered_generation = generation ? generation : 1u;
}

void frame_buffer_gone(wl_listener* listener, void*) {
    cursor_frame_obj* f = wl_container_of(listener, f, listener);
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
    f->buffer = nullptr;
    f->req.buffer = false;
    if (f->requested)
        frame_fail(f, capture_failure::unknown);
}

void frame_destroy_resource(wl_resource* resource) {
    auto* f = static_cast<cursor_frame_obj*>(wl_resource_get_user_data(resource));
    if (!f)
        return;
    frame_unwatch(f);
    if (f->session && f->session->frame == f)
        f->session->frame = nullptr;
    delete f;
}

void post_frame_error(wl_resource* r, capture_frame_error e) {
    static const char* const k_text[] = {"", "capture sent without attach_buffer",
                                         "invalid buffer damage", "capture request has been sent"};
    wl_resource_post_error(r, static_cast<std::uint32_t>(e), "%s",
                           k_text[static_cast<unsigned>(e) & 3u]);
}

const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .attach_buffer =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* buffer) {
            auto* f = static_cast<cursor_frame_obj*>(wl_resource_get_user_data(r));
            if (!f)
                return;
            if (const auto e = capture_frame_attach(f->req); e != capture_frame_error::none) {
                post_frame_error(r, e);
                return;
            }
            frame_unwatch(f);
            f->buffer = buffer;
            f->listener.notify = &frame_buffer_gone;
            wl_resource_add_destroy_listener(buffer, &f->listener);
        },
    .damage_buffer =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
            auto* f = static_cast<cursor_frame_obj*>(wl_resource_get_user_data(r));
            if (!f)
                return;
            if (const auto e = capture_frame_damage(f->req, x, y, w, h);
                e != capture_frame_error::none)
                post_frame_error(r, e);
        },
    .capture =
        [](struct wl_client*, struct wl_resource* r) {
            auto* f = static_cast<cursor_frame_obj*>(wl_resource_get_user_data(r));
            if (!f)
                return;
            if (const auto e = capture_frame_capture(f->req); e != capture_frame_error::none) {
                post_frame_error(r, e);
                return;
            }
            f->requested = true;
            if (!f->buffer) {
                frame_fail(f, capture_failure::unknown);
                return;
            }
            if (!f->session || f->session->stopped) {
                frame_fail(f, capture_failure::stopped);
                return;
            }
            // Decided by the next tick (the first frame at once there; later ones when the
            // image changes).
            capture_cursor_tick();
        },
};

void capture_destroy_resource(wl_resource* resource) {
    auto* cs = static_cast<cursor_capture_obj*>(wl_resource_get_user_data(resource));
    if (!cs)
        return;
    if (cs->frame) {
        if (cs->frame->requested && !cs->frame->done)
            frame_fail(cs->frame, capture_failure::stopped);
        cs->frame->session = nullptr;
    }
    if (cs->cursor && cs->cursor->capture == cs)
        cs->cursor->capture = nullptr;
    delete cs;
}

const struct ext_image_copy_capture_session_v1_interface capture_impl = {
    .create_frame =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            auto* cs = static_cast<cursor_capture_obj*>(wl_resource_get_user_data(resource));
            if (!cs)
                return;
            if (cs->frame) {
                wl_resource_post_error(resource,
                                       EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
                                       "create_frame sent before destroying previous frame");
                return;
            }
            wl_resource* fr = wl_resource_create(client, &ext_image_copy_capture_frame_v1_interface,
                                                 wl_resource_get_version(resource), id);
            auto* f = fr ? new (std::nothrow) cursor_frame_obj{} : nullptr;
            if (!f) {
                if (fr)
                    wl_resource_destroy(fr);
                wl_client_post_no_memory(client);
                return;
            }
            f->resource = fr;
            f->session = cs;
            wl_list_init(&f->listener.link);
            cs->frame = f;
            wl_resource_set_implementation(fr, &frame_impl, f, &frame_destroy_resource);
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void cursor_destroy_resource(wl_resource* resource) {
    auto* c = static_cast<cursor_obj*>(wl_resource_get_user_data(resource));
    if (!c)
        return;
    for (auto*& slot : g_cursors)
        if (slot == c)
            slot = nullptr;
    // "This request doesn't affect ext_image_copy_capture_frame_v1 objects": the capture
    // session lives on, stopped.
    if (c->capture) {
        capture_stop(c->capture);
        c->capture->cursor = nullptr;
    }
    delete c;
}

const struct ext_image_copy_capture_cursor_session_v1_interface cursor_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_capture_session =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            auto* c = static_cast<cursor_obj*>(wl_resource_get_user_data(resource));
            if (!c)
                return;
            if (c->capture_made) {
                wl_resource_post_error(
                    resource, EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION,
                    "get_capture_session sent twice");
                return;
            }
            c->capture_made = true;
            wl_resource* res = wl_resource_create(client, &ext_image_copy_capture_session_v1_interface,
                                                  wl_resource_get_version(resource), id);
            auto* cs = res ? new (std::nothrow) cursor_capture_obj{} : nullptr;
            if (!cs) {
                if (res)
                    wl_resource_destroy(res);
                wl_client_post_no_memory(client);
                return;
            }
            cs->resource = res;
            cs->cursor = c;
            c->capture = cs;
            wl_resource_set_implementation(res, &capture_impl, cs, &capture_destroy_resource);
            if (c->stopped) {
                capture_stop(cs);
                return;
            }
            // Constraints at once (the image at the source's scale), then frames per tick.
            capture_cursor_tick();
            if (!cs->constraints_sent && !cs->stopped) {
                // No pointer shown right now: the size of the image it will have is unknown,
                // so the plane-sized default is offered until one is.
                cs->size = {32, 32};
                ext_image_copy_capture_session_v1_send_buffer_size(res, 32, 32);
                ext_image_copy_capture_session_v1_send_shm_format(res, WL_SHM_FORMAT_ARGB8888);
                ext_image_copy_capture_session_v1_send_done(res);
                cs->constraints_sent = true;
            }
        },
};

} // namespace

void capture_cursor_session_create(wl_client* client, wl_resource* manager, uint32_t id,
                                   wl_resource* source, wl_resource* pointer) {
    (void)pointer; // one seat: its pointer is the pointer
    wl_resource* res = wl_resource_create(client, &ext_image_copy_capture_cursor_session_v1_interface,
                                          wl_resource_get_version(manager), id);
    auto* c = res ? new (std::nothrow) cursor_obj{} : nullptr;
    if (!c) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    c->resource = res;
    wl_resource_set_implementation(res, &cursor_impl, c, &cursor_destroy_resource);
    const char* identifier = "";
    (void)capture_source_of(source, c->kind, c->slot, c->output, c->toplevel, identifier);
    std::snprintf(c->identifier, sizeof(c->identifier), "%s", identifier);
    bool linked = false;
    for (auto*& slot : g_cursors) {
        if (!slot) {
            slot = c;
            linked = true;
            break;
        }
    }
    if (!linked || c->kind == capture_source_kind::none)
        c->stopped = true; // its capture session is stopped when made
}

void capture_cursor_tick() {
    const capture_host* h = g_p0_ctx ? g_p0_ctx->capture : nullptr;
    if (!h || !g_p0_ctx->cursor)
        return;
    const bool locked = g_p0_ctx->lock && g_p0_ctx->lock->state() != lock_state::unlocked;
    const double px = g_p0_ctx->cursor->x();
    const double py = g_p0_ctx->cursor->y();
    for (cursor_obj* c : g_cursors) {
        if (!c || c->stopped)
            continue;
        placement p{};
        if (!place(*c, px, py, p)) {
            cursor_stop(c);
            continue;
        }
        cursor_image_view img{};
        unsigned generation = 0;
        const bool shown =
            !locked && g_p0_ctx->cursor->image_for_scale(static_cast<unsigned>(std::lround(p.scale * 120.0)),
                                                         img, generation);
        // In the area: the image's rect intersects the source's.
        bool inside = false;
        if (shown) {
            const lx::rect2i r{p.hot_pixel.x - img.hot_x, p.hot_pixel.y - img.hot_y,
                               static_cast<int>(img.width), static_cast<int>(img.height)};
            inside = r.x < p.area.x + p.area.width && r.y < p.area.y + p.area.height &&
                     r.x + r.width > p.area.x && r.y + r.height > p.area.y;
        }
        if (!inside) {
            if (c->entered) {
                ext_image_copy_capture_cursor_session_v1_send_leave(c->resource);
                c->entered = false;
            }
        } else {
            if (!c->entered) {
                ext_image_copy_capture_cursor_session_v1_send_enter(c->resource);
                c->entered = true;
                c->position_sent = false;
                c->hotspot_sent = false;
            }
            if (!c->position_sent || c->position.x != p.hot_pixel.x ||
                c->position.y != p.hot_pixel.y) {
                c->position = p.hot_pixel;
                c->position_sent = true;
                ext_image_copy_capture_cursor_session_v1_send_position(c->resource, p.hot_pixel.x,
                                                                        p.hot_pixel.y);
            }
        }
        cursor_capture_obj* cs = c->capture;
        if (!cs || cs->stopped || !shown)
            continue;
        // Constraints follow the image's size.
        if (!cs->constraints_sent || cs->size.width != static_cast<int>(img.width) ||
            cs->size.height != static_cast<int>(img.height)) {
            cs->size = {static_cast<int>(img.width), static_cast<int>(img.height)};
            cs->constraints_sent = true;
            cs->delivered_generation = 0;
            ext_image_copy_capture_session_v1_send_buffer_size(cs->resource, img.width, img.height);
            ext_image_copy_capture_session_v1_send_shm_format(cs->resource, WL_SHM_FORMAT_ARGB8888);
            ext_image_copy_capture_session_v1_send_done(cs->resource);
        }
        // The first frame at once; later ones when the image changed (CC4).
        if (cs->frame && cs->frame->requested && !cs->frame->done &&
            (cs->delivered_generation == 0 || cs->delivered_generation != generation))
            deliver(cs, img, generation);
    }
}

void capture_cursor_toplevel_unmapped(lx::toplevel_id id) {
    for (cursor_obj* c : g_cursors)
        if (c && c->kind == capture_source_kind::toplevel && c->toplevel == id)
            cursor_stop(c);
}

#else

void capture_cursor_session_create(wl_client*, wl_resource*, uint32_t, wl_resource*, wl_resource*) {}
void capture_cursor_tick() {}
void capture_cursor_toplevel_unmapped(lx::toplevel_id) {}

#endif // LUMEN_HAS_IMAGE_CAPTURE

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
