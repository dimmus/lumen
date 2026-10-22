module;

// ext-image-capture-source-v1 and ext-image-copy-capture-v1 (staging): screen capture into
// client buffers — `grim`, xdg-desktop-portal-wlr's screencast and screenshot, OBS through
// the portal (design-app-compat-capture Part D).
//
// Sources: `ext_output_image_capture_source_manager_v1.create_source(wl_output)` and
// `ext_foreign_toplevel_image_capture_source_manager_v1.create_source(handle)` name an output
// or a mapped window (one mapping: a remapped window is another source). A source whose
// output or window is already gone gives sessions that stop at once. Destroying a source does
// not stop its sessions.
//
// Sessions (`:capture` holds the policy): constraints at creation and whenever they change
// (`buffer_size`, `shm_format` × 2, `dmabuf_device` + `dmabuf_format` × 2 on GL, `done`), one
// frame object at a time (`duplicate_frame`), frames `attach_buffer` / `damage_buffer` /
// `capture` with every protocol error (`no_buffer`, `invalid_buffer_damage`,
// `already_captured`; the manager's `invalid_option`). A captured frame is decided per tick
// (`capture_decide`): a copy job for the render thread, the lock curtain, held, or
// `failed(stopped)`. The render thread answers with a completion; a GPU copy is `ready` once
// its fence signals (the readiness poller, never a wait); a shm read-back is copied out once
// its read landed. `ready` follows `transform`, `damage` and `presentation_time` (CC6).
//
// Bounded (CC10): 64 sessions, 16 per client, 8 window sessions (each has a capture view of
// the scene); a session past a limit is created and stopped. Nothing is copied unless the
// source changed (or the frame is the session's first): an idle desktop with an idle screencast
// costs no wakeups.
//
// wlr-screencopy-unstable-v1 v3 (WS1) is a front end of the same core: each
// zwlr_screencopy_frame_v1 is a one-shot session of its output (a region is a crop of it, at
// the buffer's origin), `copy` takes the newest frame at once, `copy_with_damage` waits until
// the output changed since that client's last copy of it (`screencopy_seq_table`) and reports
// the damage. wf-recorder 0.6 and older tools speak only this. Errors: `already_used`,
// `invalid_buffer`.
//
// Session-wide, so hidden from sandboxed clients (SC2): they capture through the portal.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include <fcntl.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_EXT_IMAGE_CAPTURE_SOURCE_V1) &&                                    \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_COPY_CAPTURE_V1)
#include "ext-image-capture-source-v1-server-protocol.h"
#include "ext-image-copy-capture-v1-server-protocol.h"
#define LUMEN_HAS_IMAGE_CAPTURE 1
#if defined(LUMEN_PROTOCOL_WLR_SCREENCOPY_UNSTABLE_V1)
#include "wlr-screencopy-unstable-v1-server-protocol.h"
#define LUMEN_HAS_SCREENCOPY 1
#endif
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.scene;
import lx.trace;
import :surface;
import :surface_tree;
import :toplevel;
import :wm;
import :decoration;
import :session_lock;
import :shell_bridge;
import :output;
import :capture;
import :capture_engine;
import :content_update;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

/// Capture waits in the content-update readiness poller are keyed above every update index.
constexpr unsigned k_poll_base = 0x10000u;
/// The privacy indicator stays on this long after the last wlr-screencopy frame (CI1).
constexpr unsigned long long k_indicator_ns = 2'000'000'000ull;

capture_session_table g_sessions{};

[[nodiscard]] unsigned long long mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<unsigned long long>(ts.tv_nsec);
}

void capture_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.capture", line);
}

} // namespace

#if defined(LUMEN_HAS_IMAGE_CAPTURE)

namespace {

/// ext_image_capture_source_v1: what it names. Allocated per request.
struct source_obj {
    wl_resource* resource = nullptr;
    capture_source_kind kind = capture_source_kind::none;
    unsigned slot = 0;
    lx::output_id output{};
    lx::toplevel_id toplevel{};
    char identifier[33]{};
};

struct frame_obj;

/// ext_image_copy_capture_session_v1. `handle` 0: stopped at creation (a limit, a source
/// already gone). Allocated per request.
struct session_obj {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    unsigned handle = 0;
    unsigned generation = 0;
    frame_obj* frame = nullptr;
    unsigned serial = 0;
    /// The damage-ring seq of the last frame delivered (output sessions).
    unsigned since_seq = 0;
    /// The window's mapping (toplevel sessions).
    char identifier[33]{};
    /// The capture view (0…7; -1: none).
    int view = -1;
    bool stopped_sent = false;
    unsigned long long last_ready_ns = 0;
    /// The one-shot session of a wlr-screencopy frame (no session object of its own).
    bool screencopy = false;
    /// What its last frame painted of the pointer (paint_cursors).
    capture_cursor_memo cursor{};
};

enum class frame_state : unsigned char {
    idle,
    /// capture was sent; waiting for a decision (held, waiting for damage, queue full).
    requested,
    /// A render job is in flight.
    posted,
    /// The GPU copy is done once its fence signals.
    waiting_fence,
    /// The shm read lands when its fence signals; then the copy-out job.
    waiting_read,
    done,
};

/// Destroy listener of the attached wl_buffer (first member: the notify casts back).
struct buffer_watch {
    wl_listener listener{};
    frame_obj* frame = nullptr;
};

/// ext_image_copy_capture_frame_v1. `session` null once its session is gone. Allocated per
/// request.
struct frame_obj {
    wl_resource* resource = nullptr;
    session_obj* session = nullptr;
    capture_frame_requests req{};
    wl_resource* buffer = nullptr;
    buffer_watch watch{};
    frame_state state = frame_state::idle;
    unsigned serial = 0;
    unsigned shm_guard = 0;
    bool curtain = false;
    /// The facts of a shm frame's read (its `readback` completion).
    capture_completion meta{};
    bool has_meta = false;
    /// zwlr_screencopy_frame_v1: its events, its crop of the output (buffer pixels), whether
    /// it waits for damage, and the one-shot session it owns once copied.
    bool screencopy = false;
    bool with_damage = false;
    /// `copy`, or this client's first copy of the output: everything, at once.
    bool first_copy = true;
    lx::rect2i crop{};
    session_obj* owned = nullptr;
    /// A window job posted: the capture-view frame render composites it from (what client
    /// buffers that frame samples must outlive, `capture_view_floor`).
    unsigned view_frame = 0;
    bool view_job = false;
};

void session_release(session_obj* so);

session_obj* g_by_handle[k_capture_sessions]{};
screencopy_seq_table g_screencopy_seq{};
unsigned g_toplevel_views = 0; // bit per capture view in use
bool g_indicator = false;
lock_state g_lock_seen = lock_state::unlocked;

[[nodiscard]] capture_host* host() { return g_p0_ctx ? g_p0_ctx->capture : nullptr; }

[[nodiscard]] session_obj* session_of(unsigned handle) {
    return handle >= 1 && handle <= k_capture_sessions ? g_by_handle[handle - 1] : nullptr;
}

void post_job(const capture_job& job, bool& posted) {
    capture_host* h = host();
    if (!h || !h->channel || !h->channel->post(job)) {
        posted = false;
        return;
    }
    posted = true;
    if (h->wake_render)
        h->wake_render(h->user);
}

/// Releases a frame's shm guard entry (once render can no longer hold its handle).
void release_guard(unsigned& guard) {
    if (guard && host() && host()->guards)
        host()->guards->release(guard);
    guard = 0;
}

void buffer_unwatch(frame_obj* f) {
    if (f->buffer) {
        wl_list_remove(&f->watch.listener.link);
        wl_list_init(&f->watch.listener.link);
    }
    f->buffer = nullptr;
}

void send_failed(frame_obj* f, capture_failure reason) {
    if (!f || f->state == frame_state::done)
        return;
#if defined(LUMEN_HAS_SCREENCOPY)
    if (f->screencopy)
        zwlr_screencopy_frame_v1_send_failed(f->resource);
    else
#endif
        ext_image_copy_capture_frame_v1_send_failed(f->resource,
                                                    static_cast<std::uint32_t>(reason));
    f->state = frame_state::done;
    // A job still in flight keeps the guard until its completion says render let go.
    if (f->shm_guard && host() && host()->guards)
        host()->guards->cancel(f->shm_guard);
}

/// The window frame rect (window geometry plus a server-side frame) of a mapped window.
[[nodiscard]] lx::rect2i window_rect(const toplevel_record& rec) {
    return frame_rect(rec.geometry, window_manager::extents_of(rec));
}

/// The scale a window is captured at: what it renders at (`preferred_buffer_scale`'s rule).
[[nodiscard]] double window_scale(const toplevel_record& rec) {
    const surface_obj* s = surface_by_id(rec.surface);
    const std::uint32_t s120 = expected_scale_120(s);
    return s120 > 0 ? static_cast<double>(s120) / 120.0 : 1.0;
}

[[nodiscard]] const toplevel_record* window_of(const capture_session& cs, const session_obj* so) {
    if (!g_p0_ctx || !g_p0_ctx->toplevels)
        return nullptr;
    const toplevel_record* rec = g_p0_ctx->toplevels->find(cs.toplevel);
    if (!rec || !rec->mapped || !so || std::strcmp(rec->foreign_identifier, so->identifier) != 0)
        return nullptr;
    return rec;
}

/// The constraints the session's source asks for now; empty when the source is gone.
[[nodiscard]] capture_constraints constraints_for(const capture_session& cs,
                                                  const session_obj* so) {
    capture_host* h = host();
    if (!h)
        return {};
    const bool dmabuf = h->dmabuf && h->backend == capture_backend::gl;
    if (cs.kind == capture_source_kind::output) {
        const capture_output_info& o = h->outputs[cs.output_slot % 16];
        if (!o.live || o.id != cs.output)
            return {};
        return make_capture_constraints(o.mode, dmabuf, h->dmabuf_device);
    }
    if (cs.kind == capture_source_kind::toplevel) {
        const toplevel_record* rec = window_of(cs, so);
        if (!rec)
            return {};
        const lx::rect2i r = window_rect(*rec);
        const double s = window_scale(*rec);
        const lx::size2i size{static_cast<int>(static_cast<double>(r.width) * s + 0.5),
                              static_cast<int>(static_cast<double>(r.height) * s + 0.5)};
        if (size.width <= 0 || size.height <= 0)
            return {};
        return make_capture_constraints(size, dmabuf, h->dmabuf_device);
    }
    return {};
}

void send_constraints(session_obj* so, const capture_constraints& c) {
    ext_image_copy_capture_session_v1_send_buffer_size(so->resource,
                                                       static_cast<std::uint32_t>(c.size.width),
                                                       static_cast<std::uint32_t>(c.size.height));
    for (unsigned i = 0; i < c.shm_count; ++i)
        ext_image_copy_capture_session_v1_send_shm_format(so->resource, c.shm_formats[i]);
    if (c.dmabuf_count > 0 && c.dmabuf_device != 0) {
        wl_array dev;
        wl_array_init(&dev);
        if (void* p = wl_array_add(&dev, sizeof(dev_t))) {
            const dev_t d = static_cast<dev_t>(c.dmabuf_device);
            std::memcpy(p, &d, sizeof(d));
            ext_image_copy_capture_session_v1_send_dmabuf_device(so->resource, &dev);
        }
        wl_array_release(&dev);
        for (unsigned i = 0; i < c.dmabuf_count; ++i) {
            wl_array mods;
            wl_array_init(&mods);
            if (void* p = wl_array_add(&mods, sizeof(std::uint64_t))) {
                const std::uint64_t linear = 0;
                std::memcpy(p, &linear, sizeof(linear));
                ext_image_copy_capture_session_v1_send_dmabuf_format(so->resource,
                                                                     c.dmabuf_formats[i], &mods);
            }
            wl_array_release(&mods);
        }
    }
    ext_image_copy_capture_session_v1_send_done(so->resource);
}

void release_view(session_obj* so) {
    if (so->view < 0)
        return;
    if (capture_host* h = host(); h && h->scene)
        h->scene->remove_output_view(lx::scene::k_capture_view_base + static_cast<unsigned>(so->view));
    g_toplevel_views &= ~(1u << static_cast<unsigned>(so->view));
    so->view = -1;
}

/// The session stops (source gone): `stopped` once, a frame in progress `failed(stopped)`.
void stop_session(session_obj* so, const char* why) {
    if (!so)
        return;
    if (capture_session* cs = g_sessions.get(so->handle))
        cs->stopped = true;
    if (so->frame && so->frame->state != frame_state::idle)
        send_failed(so->frame, capture_failure::stopped);
    if (!so->stopped_sent) {
        if (so->resource)
            ext_image_copy_capture_session_v1_send_stopped(so->resource);
        so->stopped_sent = true;
        if (surface_debug()) {
            char line[128];
            std::snprintf(line, sizeof(line), "session %u stopped (%s)", so->handle, why);
            capture_log(line);
        }
    }
    release_view(so);
    if (so->handle) {
        // Whatever render still holds of it goes (a job of a gone output never runs).
        cu_poll_drop(k_poll_base + so->handle);
        capture_job forget{};
        forget.kind = capture_job_kind::forget_session;
        forget.session = so->handle;
        forget.generation = so->generation;
        bool posted = false;
        post_job(forget, posted);
    }
}

/// The client's buffer as the policy sees it.
[[nodiscard]] capture_buffer_desc describe(wl_resource* buffer) {
    capture_buffer_desc d{};
    if (!buffer)
        return d;
    if (wl_shm_buffer* shm = wl_shm_buffer_get(buffer)) {
        d.kind = capture_buffer_kind::shm;
        d.width = static_cast<unsigned>(wl_shm_buffer_get_width(shm));
        d.height = static_cast<unsigned>(wl_shm_buffer_get_height(shm));
        d.stride = static_cast<unsigned>(wl_shm_buffer_get_stride(shm));
        d.format = wl_shm_buffer_get_format(shm);
        return d;
    }
    if (buffer_obj* b = find_buffer(buffer); b && b->is_dmabuf) {
        d.kind = capture_buffer_kind::dmabuf;
        d.width = b->width;
        d.height = b->height;
        d.format = b->dmabuf.format;
        d.planes = b->dmabuf.plane_count;
        d.modifier = b->dmabuf.plane_count > 0 ? b->dmabuf.planes[0].modifier
                                               : lx::gfx::modifier_invalid;
        d.stride = b->dmabuf.plane_count > 0 ? b->dmabuf.planes[0].stride : 0;
    }
    return d;
}

/// The render target of `f`'s buffer: a dup'd dma-buf plane, or a guarded shm mapping. False
/// when no guard entry is free now (the frame waits for the next tick).
bool make_target(frame_obj* f, capture_target& t) {
    t = {};
    wl_resource* buffer = f->buffer;
    if (wl_shm_buffer* shm = wl_shm_buffer_get(buffer)) {
        capture_host* h = host();
        if (!h || !h->guards)
            return false;
        wl_shm_pool* pool = wl_shm_buffer_ref_pool(shm);
        const auto format = wl_shm_buffer_get_format(shm);
        const unsigned guard = h->guards->add(
            shm, pool, static_cast<unsigned char*>(wl_shm_buffer_get_data(shm)),
            static_cast<unsigned>(wl_shm_buffer_get_stride(shm)),
            static_cast<unsigned>(wl_shm_buffer_get_width(shm)),
            static_cast<unsigned>(wl_shm_buffer_get_height(shm)),
            format == WL_SHM_FORMAT_XRGB8888);
        if (!guard) {
            wl_shm_pool_unref(pool);
            return false;
        }
        t.width = static_cast<unsigned>(wl_shm_buffer_get_width(shm));
        t.height = static_cast<unsigned>(wl_shm_buffer_get_height(shm));
        t.shm = guard;
        f->shm_guard = guard;
        return true;
    }
    buffer_obj* b = find_buffer(buffer);
    if (!b || !b->is_dmabuf || b->dmabuf.plane_count == 0)
        return false;
    t.dmabuf = true;
    t.width = b->width;
    t.height = b->height;
    t.buffer_key = static_cast<unsigned>(b->id.id());
    t.fd = ::fcntl(b->dmabuf.planes[0].fd.get(), F_DUPFD_CLOEXEC, 0);
    t.offset = b->dmabuf.planes[0].offset;
    t.stride = b->dmabuf.planes[0].stride;
    t.format = b->dmabuf.format;
    t.modifier = b->dmabuf.planes[0].modifier;
    b->capture_imported = true;
    return t.fd >= 0;
}

/// The environment `cs` is decided in.
[[nodiscard]] capture_env env_for(const capture_session& cs, const session_obj* so) {
    capture_env env{};
    capture_host* h = host();
    if (g_p0_ctx && g_p0_ctx->lock && g_p0_ctx->lock->state() != lock_state::unlocked) {
        env.locked = true;
        if (auto* lock_res = static_cast<wl_resource*>(g_p0_ctx->lock->current()))
            env.locker_client = wire_client_id(wl_resource_get_client(lock_res));
    }
    if (cs.kind == capture_source_kind::output) {
        const capture_output_info* o = h ? &h->outputs[cs.output_slot % 16] : nullptr;
        env.source_gone = !o || !o->live || o->id != cs.output;
        env.held = o && o->held;
        env.lock_scene_shown = o && o->lock_shown;
    } else {
        env.source_gone = window_of(cs, so) == nullptr;
        // A window's content cannot change while the VT is away (CC8); a dark output does not
        // hold a window that may be on another one.
        env.held = h && h->paused;
    }
    return env;
}

/// The window session's capture view, configured for the window as it is now. False when
/// none is free.
bool configure_view(session_obj* so, const capture_session& cs) {
    capture_host* h = host();
    if (!h || !h->scene || !g_p0_ctx || !g_p0_ctx->tree)
        return false;
    const toplevel_record* rec = window_of(cs, so);
    if (!rec)
        return false;
    if (so->view < 0) {
        for (unsigned v = 0; v < k_capture_toplevel_views; ++v) {
            if (!(g_toplevel_views & (1u << v))) {
                g_toplevel_views |= 1u << v;
                so->view = static_cast<int>(v);
                break;
            }
        }
        if (so->view < 0)
            return false;
    }
    const lx::rect2i r = window_rect(*rec);
    lx::scene::output_view_config c{};
    c.logical = r;
    c.projection.origin = {r.x, r.y};
    c.projection.scale = window_scale(*rec);
    c.projection.size = cs.constraints.size;
    c.projection.transform = lx::buffer_transform::normal;
    c.root = g_p0_ctx->tree->container(rec->surface);
    if (!c.root)
        return false;
    h->scene->set_output_view(lx::scene::k_capture_view_base + static_cast<unsigned>(so->view), c);
    return true;
}

/// Decides `so`'s requested frame and, when it gets one, posts its job. `in_tick`: a capture
/// tick is building frame `host()->frame` (window sessions publish their view only then).
void schedule(session_obj* so, bool in_tick) {
    frame_obj* f = so ? so->frame : nullptr;
    if (!f || f->state != frame_state::requested)
        return;
    capture_session* cs = g_sessions.get(so->handle);
    if (!cs) {
        send_failed(f, capture_failure::stopped);
        return;
    }
    const capture_action action = capture_decide(*cs, env_for(*cs, so));
    if (action == capture_action::stop) {
        stop_session(so, "source gone");
        return;
    }
    if (action == capture_action::wait)
        return;
    // The buffer must match what the session asks for now (a mode change, a resize).
    if (!capture_buffer_fits(cs->constraints, describe(f->buffer))) {
        send_failed(f, capture_failure::buffer_constraints);
        return;
    }
    capture_host* h = host();
    capture_job job{};
    job.session = so->handle;
    job.generation = so->generation;
    job.serial = f->serial;
    job.paint_cursors = cs->paint_cursors;
    job.client_damage = f->req.damage;
    job.cursor = so->cursor;
    job.lock_epoch = h && h->channel ? h->channel->lock_epoch.load(std::memory_order_relaxed) : 0;
    if (action == capture_action::curtain) {
        job.kind = capture_job_kind::curtain;
        job.full = true;
    } else if (cs->kind == capture_source_kind::output) {
        job.kind = capture_job_kind::output;
        job.output_slot = cs->output_slot;
        job.full = !cs->delivered || cs->full_damage;
        job.since_seq = so->since_seq;
        if (f->screencopy) {
            // `copy` takes the newest frame now; `copy_with_damage` waits for the output to
            // change since this client's last copy (its first copy is full).
            job.crop = f->crop;
            job.full = f->first_copy;
        }
        const capture_env env = env_for(*cs, so);
        if (env.locked) {
            // The lock holder sees frames built locked only (capture_decide let it through).
            job.has_min_frame = true;
            job.min_frame = h ? h->outputs[cs->output_slot % 16].lock_frame : 0;
        }
    } else {
        if (!in_tick || !h || !h->scene)
            return; // published by the next capture tick
        if (!configure_view(so, *cs)) {
            stop_session(so, "no capture view");
            return;
        }
        const unsigned view = lx::scene::k_capture_view_base + static_cast<unsigned>(so->view);
        job.full = !cs->delivered || cs->full_damage;
        if (!job.full && h->scene->pending_damage(view).empty())
            return; // the window has not changed since the last frame
        if (!h->scene->commit_frame(view, h->frame))
            return; // render still holds every slot of the view: the next tick
        // The view's frame samples the client buffers on screen now, like an output's: the
        // buffer lifecycle tags them with it.
        if (g_p0_ctx && g_p0_ctx->lifecycle)
            g_p0_ctx->lifecycle->on_frame_submitted(h->frame);
        job.kind = capture_job_kind::toplevel;
        job.view = view;
        job.view_frame = h->frame;
        job.view_origin = h->scene->view_config(view).projection.origin;
        job.view_scale = h->scene->view_config(view).projection.scale;
    }
    if (!make_target(f, job.target)) {
        if (f->shm_guard)
            release_guard(f->shm_guard);
        if (job.target.fd >= 0)
            ::close(job.target.fd);
        return; // a guard frees up when a frame completes: the next tick
    }
    bool posted = false;
    post_job(job, posted);
    if (!posted) {
        if (job.target.fd >= 0)
            ::close(job.target.fd);
        release_guard(f->shm_guard);
        return;
    }
    f->curtain = job.kind == capture_job_kind::curtain;
    f->view_job = job.kind == capture_job_kind::toplevel;
    f->view_frame = job.view_frame;
    f->state = frame_state::posted;
}

/// `transform`, `damage`, `presentation_time`, `ready` (CC6) and the session's bookkeeping.
void send_ready(frame_obj* f, const capture_completion& meta) {
    session_obj* so = f->session;
    capture_session* cs = so ? g_sessions.get(so->handle) : nullptr;
    capture_host* h = host();
    std::uint32_t transform = 0;
    unsigned long long when = meta.time_ns;
    if (cs && cs->kind == capture_source_kind::output && h) {
        transform = static_cast<std::uint32_t>(h->outputs[cs->output_slot % 16].transform);
        if (h->predict && !f->curtain) {
            if (const unsigned long long p = h->predict(h->user, cs->output_slot, meta.time_ns))
                when = p;
        }
    }
    if (when == 0)
        when = mono_ns();
#if defined(LUMEN_HAS_SCREENCOPY)
    if (f->screencopy) {
        // flags, damage (copy_with_damage), ready.
        zwlr_screencopy_frame_v1_send_flags(f->resource, 0);
        if (f->with_damage && wl_resource_get_version(f->resource) >= 2) {
            const lx::gfx::damage_region& dd = meta.damage;
            if (dd.full || f->curtain || f->first_copy) {
                zwlr_screencopy_frame_v1_send_damage(f->resource, 0, 0,
                                                     static_cast<std::uint32_t>(f->crop.width),
                                                     static_cast<std::uint32_t>(f->crop.height));
            } else {
                for (unsigned i = 0; i < dd.count; ++i)
                    zwlr_screencopy_frame_v1_send_damage(
                        f->resource, static_cast<std::uint32_t>(dd.rects[i].x),
                        static_cast<std::uint32_t>(dd.rects[i].y),
                        static_cast<std::uint32_t>(dd.rects[i].width),
                        static_cast<std::uint32_t>(dd.rects[i].height));
            }
        }
        const unsigned long long ssec = when / 1'000'000'000ull;
        zwlr_screencopy_frame_v1_send_ready(f->resource, static_cast<std::uint32_t>(ssec >> 32),
                                            static_cast<std::uint32_t>(ssec & 0xFFFFFFFFull),
                                            static_cast<std::uint32_t>(when % 1'000'000'000ull));
        f->state = frame_state::done;
        if (cs) {
            cs->delivered = true;
            if (f->curtain)
                cs->curtained = true;
            else if (cs->kind == capture_source_kind::output && so && f->with_damage)
                // Only copy_with_damage moves the client's damage point (wlroots: a plain copy
                // leaves its accumulator alone).
                g_screencopy_seq.set(cs->client, cs->output, meta.seq, meta.cursor);
        }
        if (so && !f->curtain)
            so->last_ready_ns = mono_ns();
        return;
    }
#endif
    ext_image_copy_capture_frame_v1_send_transform(f->resource, transform);
    const lx::gfx::damage_region& d = meta.damage;
    if (d.full || f->curtain || (cs && !cs->delivered)) {
        // The first frame always carries full damage.
        const capture_constraints* c = cs ? &cs->constraints : nullptr;
        ext_image_copy_capture_frame_v1_send_damage(f->resource, 0, 0,
                                                    c ? c->size.width : 0x7FFFFFFF,
                                                    c ? c->size.height : 0x7FFFFFFF);
    } else {
        for (unsigned i = 0; i < d.count; ++i)
            ext_image_copy_capture_frame_v1_send_damage(f->resource, d.rects[i].x, d.rects[i].y,
                                                        d.rects[i].width, d.rects[i].height);
    }
    const unsigned long long sec = when / 1'000'000'000ull;
    ext_image_copy_capture_frame_v1_send_presentation_time(
        f->resource, static_cast<std::uint32_t>(sec >> 32),
        static_cast<std::uint32_t>(sec & 0xFFFFFFFFull),
        static_cast<std::uint32_t>(when % 1'000'000'000ull));
    ext_image_copy_capture_frame_v1_send_ready(f->resource);
    f->state = frame_state::done;
    if (so)
        so->cursor = meta.cursor;
    if (cs) {
        cs->delivered = true;
        cs->full_damage = false;
        if (f->curtain) {
            cs->curtained = true;
        } else if (cs->kind == capture_source_kind::output) {
            so->since_seq = meta.seq;
        }
    }
    if (so && !f->curtain)
        so->last_ready_ns = mono_ns();
    if (surface_debug() && so) {
        char line[160];
        std::snprintf(line, sizeof(line), "frame ready session=%u serial=%u damage=%s%u frame=%u%s",
                      so->handle, f->serial, d.full ? "full/" : "", d.full ? 0u : d.count,
                      meta.frame, f->curtain ? " curtain" : "");
        capture_log(line);
    }
}

// ── frame ────────────────────────────────────────────────────────────────────

void frame_buffer_destroyed(wl_listener* listener, void*) {
    auto* watch = reinterpret_cast<buffer_watch*>(listener);
    frame_obj* f = watch->frame;
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
    if (!f)
        return;
    f->buffer = nullptr;
    f->req.buffer = false;
    // "unknown: … buffer destroyed before the copy": a copy not finished fails; render does
    // not touch the memory after this returns (the guard).
    if (f->state != frame_state::idle && f->state != frame_state::done)
        send_failed(f, capture_failure::unknown);
}

void frame_destroy_resource(wl_resource* resource) {
    auto* f = static_cast<frame_obj*>(wl_resource_get_user_data(resource));
    if (!f)
        return;
    buffer_unwatch(f);
    if (f->shm_guard && host() && host()->guards) {
        // Render may still hold the handle: cancelled now, released by its completion.
        host()->guards->cancel(f->shm_guard);
    }
    if (f->session) {
        if (f->session->frame == f)
            f->session->frame = nullptr;
        // A fence wait of this frame is of no use any more.
        if (g_p0_ctx && (f->state == frame_state::waiting_fence ||
                         f->state == frame_state::waiting_read))
            cu_poll_drop(k_poll_base + f->session->handle);
    }
    if (f->owned) {
        // A screencopy frame's one-shot session goes with it.
        f->owned->frame = nullptr;
        session_release(f->owned);
        delete f->owned;
        f->owned = nullptr;
    }
    delete f;
}

frame_obj* frame_from(wl_resource* r) {
    return static_cast<frame_obj*>(wl_resource_get_user_data(r));
}

void post_frame_error(wl_resource* r, capture_frame_error e) {
    switch (e) {
    case capture_frame_error::no_buffer:
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
                               "capture sent without attach_buffer");
        break;
    case capture_frame_error::invalid_buffer_damage:
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
                               "invalid buffer damage");
        break;
    case capture_frame_error::already_captured:
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "capture request has been sent");
        break;
    case capture_frame_error::none:
        break;
    }
}

const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .attach_buffer =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* buffer) {
            frame_obj* f = frame_from(r);
            if (!f)
                return;
            if (const auto e = capture_frame_attach(f->req); e != capture_frame_error::none) {
                post_frame_error(r, e);
                return;
            }
            buffer_unwatch(f);
            f->buffer = buffer;
            f->watch.frame = f;
            f->watch.listener.notify = &frame_buffer_destroyed;
            wl_resource_add_destroy_listener(buffer, &f->watch.listener);
        },
    .damage_buffer =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
            frame_obj* f = frame_from(r);
            if (!f)
                return;
            if (const auto e = capture_frame_damage(f->req, x, y, w, h);
                e != capture_frame_error::none)
                post_frame_error(r, e);
        },
    .capture =
        [](struct wl_client*, struct wl_resource* r) {
            frame_obj* f = frame_from(r);
            if (!f)
                return;
            if (const auto e = capture_frame_capture(f->req); e != capture_frame_error::none) {
                post_frame_error(r, e);
                return;
            }
            if (!f->buffer) {
                // Attached, then destroyed: the buffer is gone before the copy.
                f->state = frame_state::requested;
                send_failed(f, capture_failure::unknown);
                return;
            }
            session_obj* so = f->session;
            capture_session* cs = so ? g_sessions.get(so->handle) : nullptr;
            f->state = frame_state::requested;
            if (!cs || cs->stopped) {
                send_failed(f, capture_failure::stopped);
                return;
            }
            f->serial = ++so->serial;
            schedule(so, false);
            // A window session publishes its view at the next tick, which its window's
            // output (or any) has to have.
            if (f->state == frame_state::requested && host() && host()->request_frame)
                host()->request_frame(host()->user, ~0u);
        },
};

// ── session ──────────────────────────────────────────────────────────────────

/// A session goes (its object, or a screencopy frame owning it): its frame turns inert, render
/// lets go of it, its slot is freed. The caller deletes it.
void session_release(session_obj* so) {
    if (so->frame) {
        // "This request doesn't affect ext_image_copy_capture_frame_v1 objects": the frame
        // lives on, inert — a capture in progress fails as stopped.
        if (so->frame->state != frame_state::idle && so->frame->state != frame_state::done)
            send_failed(so->frame, capture_failure::stopped);
        so->frame->session = nullptr;
        so->frame = nullptr;
    }
    release_view(so);
    if (so->handle) {
        cu_poll_drop(k_poll_base + so->handle);
        g_by_handle[so->handle - 1] = nullptr;
        capture_job forget{};
        forget.kind = capture_job_kind::forget_session;
        forget.session = so->handle;
        forget.generation = so->generation;
        bool posted = false;
        post_job(forget, posted);
        g_sessions.close(so->handle);
        if (capture_host* h = host(); h && h->channel)
            h->channel->sessions.store(g_sessions.count(), std::memory_order_release);
        so->handle = 0;
    }
}

void session_destroy_resource(wl_resource* resource) {
    auto* so = static_cast<session_obj*>(wl_resource_get_user_data(resource));
    if (!so)
        return;
    session_release(so);
    delete so;
}

const struct ext_image_copy_capture_session_v1_interface session_impl = {
    .create_frame =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            auto* so = static_cast<session_obj*>(wl_resource_get_user_data(resource));
            if (!so)
                return;
            if (so->frame) {
                wl_resource_post_error(resource,
                                       EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
                                       "create_frame sent before destroying previous frame");
                return;
            }
            wl_resource* fr = wl_resource_create(client, &ext_image_copy_capture_frame_v1_interface,
                                                 wl_resource_get_version(resource), id);
            if (!fr) {
                wl_client_post_no_memory(client);
                return;
            }
            auto* f = new (std::nothrow) frame_obj{};
            if (!f) {
                wl_resource_destroy(fr);
                wl_client_post_no_memory(client);
                return;
            }
            f->resource = fr;
            f->session = so;
            wl_list_init(&f->watch.listener.link);
            so->frame = f;
            wl_resource_set_implementation(fr, &frame_impl, f, &frame_destroy_resource);
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

// ── sources ──────────────────────────────────────────────────────────────────

void source_destroy_resource(wl_resource* resource) {
    delete static_cast<source_obj*>(wl_resource_get_user_data(resource));
}

const struct ext_image_capture_source_v1_interface source_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

source_obj* make_source(wl_client* client, wl_resource* manager, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &ext_image_capture_source_v1_interface,
                                          wl_resource_get_version(manager), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return nullptr;
    }
    auto* s = new (std::nothrow) source_obj{};
    if (!s) {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return nullptr;
    }
    s->resource = res;
    wl_resource_set_implementation(res, &source_impl, s, &source_destroy_resource);
    return s;
}

const struct ext_output_image_capture_source_manager_v1_interface output_source_manager_impl = {
    .create_source =
        [](struct wl_client* client, struct wl_resource* manager, uint32_t id,
           struct wl_resource* output) {
            source_obj* s = make_source(client, manager, id);
            if (!s)
                return;
            const int slot = output_resource_slot(output);
            capture_host* h = host();
            if (slot >= 0 && h && h->outputs[slot].live) {
                s->kind = capture_source_kind::output;
                s->slot = static_cast<unsigned>(slot);
                s->output = h->outputs[slot].id;
            }
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct ext_foreign_toplevel_image_capture_source_manager_v1_interface
    toplevel_source_manager_impl = {
        .create_source =
            [](struct wl_client* client, struct wl_resource* manager, uint32_t id,
               struct wl_resource* handle) {
                source_obj* s = make_source(client, manager, id);
                if (!s)
                    return;
                const char* identifier = "";
                const lx::toplevel_id t = foreign_toplevel_of(handle, identifier);
                if (t) {
                    s->kind = capture_source_kind::toplevel;
                    s->toplevel = t;
                    std::snprintf(s->identifier, sizeof(s->identifier), "%s", identifier);
                }
            },
        .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

// ── manager ──────────────────────────────────────────────────────────────────

void create_session(struct wl_client* client, struct wl_resource* manager, uint32_t id,
                    struct wl_resource* source_res, uint32_t options) {
    if (options & ~static_cast<uint32_t>(EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS)) {
        wl_resource_post_error(manager, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
                               "invalid option flag");
        return;
    }
    wl_resource* res = wl_resource_create(client, &ext_image_copy_capture_session_v1_interface,
                                          wl_resource_get_version(manager), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* so = new (std::nothrow) session_obj{};
    if (!so) {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    so->resource = res;
    so->client = client;
    wl_resource_set_implementation(res, &session_impl, so, &session_destroy_resource);

    const auto* src = source_res ? static_cast<const source_obj*>(wl_resource_get_user_data(source_res))
                                 : nullptr;
    capture_host* h = host();
    const char* why = nullptr;
    if (!h || h->backend == capture_backend::none)
        why = "no capture backend";
    else if (!src || src->kind == capture_source_kind::none)
        why = "source gone";
    else if (src->kind == capture_source_kind::toplevel && h->backend == capture_backend::vulkan)
        why = "window capture needs the GL or CPU backend";
    unsigned handle = 0;
    if (!why) {
        handle = g_sessions.open(wire_client_id(client), src->kind);
        if (!handle)
            why = "session limit";
    }
    if (handle) {
        capture_session* cs = g_sessions.get(handle);
        so->handle = handle;
        so->generation = cs->generation;
        g_by_handle[handle - 1] = so;
        cs->output_slot = src->slot;
        cs->output = src->output;
        cs->toplevel = src->toplevel;
        cs->paint_cursors = (options & EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) != 0;
        std::memcpy(so->identifier, src->identifier, sizeof(so->identifier));
        cs->constraints = constraints_for(*cs, so);
        if (h->channel)
            h->channel->sessions.store(g_sessions.count(), std::memory_order_release);
        if (!cs->constraints.valid()) {
            why = "source gone";
        } else {
            send_constraints(so, cs->constraints);
        }
    }
    if (surface_debug()) {
        char line[192];
        std::snprintf(line, sizeof(line), "session %u client=%u kind=%s paint_cursors=%d%s%s",
                      handle, wire_client_id(client),
                      !src                                          ? "none"
                      : src->kind == capture_source_kind::output    ? "output"
                      : src->kind == capture_source_kind::toplevel  ? "toplevel"
                                                                    : "none",
                      (options & 1u) ? 1 : 0, why ? " stopped: " : "", why ? why : "");
        capture_log(line);
    }
    if (why)
        stop_session(so, why);
}

const struct ext_image_copy_capture_manager_v1_interface manager_impl = {
    .create_session = &create_session,
    .create_pointer_cursor_session =
        [](struct wl_client* client, struct wl_resource* manager, uint32_t id,
           struct wl_resource* source, struct wl_resource* pointer) {
            capture_cursor_session_create(client, manager, id, source, pointer);
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

// ── wlr-screencopy-unstable-v1 v3 (WS1) ──────────────────────────────────────

#if defined(LUMEN_HAS_SCREENCOPY)

/// The output pixels of a logical region of output `o` (floor / ceil of the scaled box, then
/// the output's transform), clipped to its mode.
lx::rect2i output_region(const capture_output_info& o, int x, int y, int w, int h) {
    const double s = o.scale > 0.0 ? o.scale : 1.0;
    const double fx0 = std::floor((static_cast<double>(x) - o.origin.x) * s);
    const double fy0 = std::floor((static_cast<double>(y) - o.origin.y) * s);
    const double fx1 = std::ceil((static_cast<double>(x) + w - o.origin.x) * s);
    const double fy1 = std::ceil((static_cast<double>(y) + h - o.origin.y) * s);
    const bool swap = (static_cast<unsigned>(o.transform) & 1u) != 0;
    const int cw = swap ? o.mode.height : o.mode.width;
    const int ch = swap ? o.mode.width : o.mode.height;
    lx::rect2i r{static_cast<int>(fx0), static_cast<int>(fy0), static_cast<int>(fx1 - fx0),
                 static_cast<int>(fy1 - fy0)};
    const int x0 = r.x < 0 ? 0 : r.x;
    const int y0 = r.y < 0 ? 0 : r.y;
    const int x1 = r.x + r.width > cw ? cw : r.x + r.width;
    const int y1 = r.y + r.height > ch ? ch : r.y + r.height;
    if (x1 <= x0 || y1 <= y0)
        return {};
    r = {x0, y0, x1 - x0, y1 - y0};
    if (o.transform != lx::buffer_transform::normal)
        r = lx::gfx::transform_rect(r, o.transform, cw, ch);
    return r;
}

void screencopy_copy(wl_client* client, wl_resource* resource, wl_resource* buffer,
                     bool with_damage) {
    frame_obj* f = frame_from(resource);
    if (!f)
        return;
    if (f->req.captured) {
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
                               "frame already used");
        return;
    }
    // The buffer must be one the frame offered: its size, XRGB/ARGB shm with the advertised
    // stride or more, or a single-plane XR24/AR24 LINEAR dma-buf.
    const capture_buffer_desc d = describe(buffer);
    capture_host* h = host();
    const capture_constraints c = make_capture_constraints(
        {f->crop.width, f->crop.height}, h && h->dmabuf && h->backend == capture_backend::gl,
        h ? h->dmabuf_device : 0);
    if (!capture_buffer_fits(c, d)) {
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
                               "invalid buffer");
        return;
    }
    f->req.captured = true;
    f->req.buffer = true;
    f->with_damage = with_damage;
    f->buffer = buffer;
    f->watch.frame = f;
    f->watch.listener.notify = &frame_buffer_destroyed;
    wl_resource_add_destroy_listener(buffer, &f->watch.listener);
    // The one-shot session.
    session_obj* so = f->owned;
    const unsigned cid = wire_client_id(client);
    capture_session* cs = so ? g_sessions.get(so->handle) : nullptr;
    f->state = frame_state::requested;
    if (!cs) {
        send_failed(f, capture_failure::unknown);
        return;
    }
    // copy_with_damage reports and waits for what changed since this client's last copy of the
    // output (its first copy is full).
    unsigned seq = 0;
    capture_cursor_memo memo{};
    f->first_copy = !with_damage || !g_screencopy_seq.get(cid, cs->output, seq, memo);
    so->since_seq = seq;
    so->cursor = with_damage ? memo : capture_cursor_memo{};
    f->serial = ++so->serial;
    schedule(so, false);
}

const struct zwlr_screencopy_frame_v1_interface screencopy_frame_impl = {
    .copy = [](struct wl_client* client, struct wl_resource* r,
               struct wl_resource* buffer) { screencopy_copy(client, r, buffer, false); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .copy_with_damage = [](struct wl_client* client, struct wl_resource* r,
                           struct wl_resource* buffer) { screencopy_copy(client, r, buffer, true); },
};

void screencopy_capture(wl_client* client, wl_resource* manager, uint32_t id, int32_t overlay,
                        wl_resource* output, bool region, int32_t x, int32_t y, int32_t w,
                        int32_t h) {
    wl_resource* res = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface,
                                          wl_resource_get_version(manager), id);
    auto* f = res ? new (std::nothrow) frame_obj{} : nullptr;
    if (!f) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    f->resource = res;
    f->screencopy = true;
    wl_list_init(&f->watch.listener.link);
    wl_resource_set_implementation(res, &screencopy_frame_impl, f, &frame_destroy_resource);
    capture_host* hst = host();
    const int slot = output_resource_slot(output);
    const capture_output_info* o =
        hst && slot >= 0 && slot < 16 && hst->outputs[slot].live ? &hst->outputs[slot] : nullptr;
    if (o) {
        f->crop = region ? output_region(*o, x, y, w, h) : lx::rect2i{0, 0, o->mode.width, o->mode.height};
        // An empty region (w or h ≤ 0, or off the output) captures nothing.
    }
    unsigned handle = 0;
    if (o && f->crop.width > 0 && f->crop.height > 0)
        handle = g_sessions.open(wire_client_id(client), capture_source_kind::output);
    if (!handle) {
        f->state = frame_state::requested;
        send_failed(f, capture_failure::unknown);
        return;
    }
    auto* so = new (std::nothrow) session_obj{};
    if (!so) {
        g_sessions.close(handle);
        f->state = frame_state::requested;
        send_failed(f, capture_failure::unknown);
        return;
    }
    capture_session* cs = g_sessions.get(handle);
    so->client = client;
    so->handle = handle;
    so->generation = cs->generation;
    so->screencopy = true;
    so->frame = f;
    g_by_handle[handle - 1] = so;
    f->session = so;
    f->owned = so;
    cs->screencopy = true;
    cs->output_slot = static_cast<unsigned>(slot);
    cs->output = o->id;
    cs->paint_cursors = overlay != 0;
    cs->constraints = make_capture_constraints({f->crop.width, f->crop.height},
                                               hst->dmabuf && hst->backend == capture_backend::gl,
                                               hst->dmabuf_device);
    if (hst->channel)
        hst->channel->sessions.store(g_sessions.count(), std::memory_order_release);
    // buffer, linux_dmabuf (v3), buffer_done (v3).
    zwlr_screencopy_frame_v1_send_buffer(res, WL_SHM_FORMAT_XRGB8888,
                                         static_cast<std::uint32_t>(f->crop.width),
                                         static_cast<std::uint32_t>(f->crop.height),
                                         static_cast<std::uint32_t>(f->crop.width) * 4u);
    if (wl_resource_get_version(res) >= 3) {
        if (cs->constraints.dmabuf_count > 0)
            zwlr_screencopy_frame_v1_send_linux_dmabuf(res, cs->constraints.dmabuf_formats[0],
                                                       static_cast<std::uint32_t>(f->crop.width),
                                                       static_cast<std::uint32_t>(f->crop.height));
        zwlr_screencopy_frame_v1_send_buffer_done(res);
    }
}

const struct zwlr_screencopy_manager_v1_interface screencopy_manager_impl = {
    .capture_output =
        [](struct wl_client* client, struct wl_resource* manager, uint32_t id, int32_t overlay,
           struct wl_resource* output) {
            screencopy_capture(client, manager, id, overlay, output, false, 0, 0, 0, 0);
        },
    .capture_output_region =
        [](struct wl_client* client, struct wl_resource* manager, uint32_t id, int32_t overlay,
           struct wl_resource* output, int32_t x, int32_t y, int32_t w, int32_t h) {
            screencopy_capture(client, manager, id, overlay, output, true, x, y, w, h);
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

#endif // LUMEN_HAS_SCREENCOPY

} // namespace

void bind_output_capture_source_manager(wayland::client_connection& client, unsigned id,
                                        int version) {
    auto res = wayland::resource::create(client, &ext_output_image_capture_source_manager_v1_interface,
                                         version, id);
    if (res)
        res.set_implementation(&output_source_manager_impl, g_p0_ctx, nullptr);
}

void bind_toplevel_capture_source_manager(wayland::client_connection& client, unsigned id,
                                          int version) {
    auto res = wayland::resource::create(
        client, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&toplevel_source_manager_impl, g_p0_ctx, nullptr);
}

#if defined(LUMEN_HAS_SCREENCOPY)
void bind_screencopy_manager(wayland::client_connection& client, unsigned id, int version) {
    auto res = wayland::resource::create(client, &zwlr_screencopy_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&screencopy_manager_impl, g_p0_ctx, nullptr);
}
#endif

void bind_image_copy_capture_manager(wayland::client_connection& client, unsigned id, int version) {
    auto res = wayland::resource::create(client, &ext_image_copy_capture_manager_v1_interface,
                                         version, id);
    if (res)
        res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

bool capture_source_of(wl_resource* source, capture_source_kind& kind, unsigned& slot,
                       lx::output_id& output, lx::toplevel_id& toplevel, const char*& identifier) {
    kind = capture_source_kind::none;
    identifier = "";
    if (!source || !wl_resource_instance_of(source, &ext_image_capture_source_v1_interface,
                                            &source_impl))
        return false;
    const auto* s = static_cast<const source_obj*>(wl_resource_get_user_data(source));
    if (!s)
        return false;
    kind = s->kind;
    slot = s->slot;
    output = s->output;
    toplevel = s->toplevel;
    identifier = s->identifier;
    return true;
}

#endif // LUMEN_HAS_IMAGE_CAPTURE

// ── the compositor's entry points ────────────────────────────────────────────

void capture_toplevel_unmapped(lx::toplevel_id id) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    g_sessions.for_each([&](unsigned handle, capture_session& cs) {
        if (cs.kind == capture_source_kind::toplevel && cs.toplevel == id)
            stop_session(session_of(handle), "window unmapped");
    });
#else
    (void)id;
#endif
    capture_cursor_toplevel_unmapped(id);
}

void capture_fence_signalled(unsigned key, unsigned serial) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    if (key < k_poll_base)
        return;
    session_obj* so = session_of(key - k_poll_base);
    frame_obj* f = so ? so->frame : nullptr;
    if (!f || f->serial != serial)
        return;
    if (f->state == frame_state::waiting_fence) {
        send_ready(f, f->meta);
    } else if (f->state == frame_state::waiting_read) {
        capture_job fin{};
        fin.kind = capture_job_kind::finish;
        fin.session = so->handle;
        fin.generation = so->generation;
        fin.serial = f->serial;
        fin.target.shm = f->shm_guard;
        bool posted = false;
        post_job(fin, posted);
        if (posted)
            f->state = frame_state::posted;
        else
            send_failed(f, capture_failure::unknown);
    }
    wl_client_flush(wl_resource_get_client(f->resource));
#else
    (void)key;
    (void)serial;
#endif
}

void capture_buffer_gone(wl_resource* buffer, buffer_obj* b) {
    (void)buffer;
    if (!b || !b->capture_imported || !g_p0_ctx || !g_p0_ctx->capture)
        return;
    capture_job forget{};
    forget.kind = capture_job_kind::forget_buffer;
    forget.target.buffer_key = static_cast<unsigned>(b->id.id());
    if (capture_channel* ch = g_p0_ctx->capture->channel)
        (void)ch->post(forget); // full: the import is evicted with its LRU slot instead
}

void bind_capture_guards(p0_protocol_context& ctx) {
    if (!ctx.capture || !ctx.capture->guards)
        return;
    capture_shm_guards::hooks h{};
    h.begin_access = [](void* shm) { wl_shm_buffer_begin_access(static_cast<wl_shm_buffer*>(shm)); };
    h.end_access = [](void* shm) { wl_shm_buffer_end_access(static_cast<wl_shm_buffer*>(shm)); };
    h.unref_pool = [](void* pool) { wl_shm_pool_unref(static_cast<wl_shm_pool*>(pool)); };
    ctx.capture->guards->bind(h);
}

bool capture_paces_surface(lx::surface_id surface) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    if (g_toplevel_views == 0 || !g_p0_ctx || !g_p0_ctx->tree || !g_p0_ctx->toplevels)
        return false;
    const lx::surface_id root = g_p0_ctx->tree->root_of(surface);
    const toplevel_record* rec = g_p0_ctx->toplevels->find_by_surface(root ? root : surface);
    if (!rec)
        return false;
    bool paced = false;
    g_sessions.for_each([&](unsigned handle, capture_session& cs) {
        const session_obj* so = session_of(handle);
        if (cs.kind == capture_source_kind::toplevel && cs.toplevel == rec->id && so &&
            so->frame && so->frame->state == frame_state::requested)
            paced = true;
    });
    return paced;
#else
    (void)surface;
    return false;
#endif
}

} // namespace lx::compositor::p0

void lx::compositor::capture_complete(p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    using namespace lx::compositor::p0;
    capture_host* h = ctx.capture;
    if (!h || !h->channel || !h->channel->has_completions())
        return;
    capture_completion done[32];
    for (;;) {
        const unsigned n = h->channel->drain(done, 32);
        for (unsigned i = 0; i < n; ++i) {
            capture_completion& c = done[i];
            if ((c.result == capture_result::ready || c.result == capture_result::fence) &&
                c.lock_epoch != h->channel->lock_epoch.load(std::memory_order_acquire)) {
                // Copied before a lock change it would leak past (a worker read can land
                // after the lock): decided again — the curtain, or content once unlocked.
                if (c.fence_fd >= 0)
                    ::close(c.fence_fd);
                c.fence_fd = -1;
                c.result = capture_result::cancelled;
            }
            session_obj* so = session_of(c.session);
            if (so && so->generation != c.generation)
                so = nullptr;
            frame_obj* f = so && so->frame && so->frame->serial == c.serial ? so->frame : nullptr;
            // Someone still waits for this completion: the frame's job (or its copy-out).
            const bool live = f && (f->state == frame_state::posted ||
                                    (c.finished_read && f->state == frame_state::waiting_read));
            // Render let go of the guard handle unless a copy-out still follows.
            if (c.shm && !(live && c.result == capture_result::readback)) {
                h->guards->release(c.shm);
                if (f && f->shm_guard == c.shm)
                    f->shm_guard = 0;
            }
            if (!live) {
                if (c.fence_fd >= 0)
                    ::close(c.fence_fd);
                continue;
            }
            f->view_job = false; // render is past its view frame
            switch (c.result) {
            case capture_result::cancelled:
                f->state = frame_state::requested;
                schedule(so, false);
                break;
            case capture_result::failed:
                send_failed(f, c.reason);
                break;
            case capture_result::fence:
                f->meta = c;
                f->meta.fence_fd = -1;
                // `ready` only once the copy into the client's buffer has completed. Each
                // session has at most one fence waiting, and the poller keeps an entry per
                // session for them, so only epoll itself can refuse it: the frame fails then
                // (the client asks again) rather than reach the client unfinished.
                static_assert(k_capture_sessions <= lx::compositor::readiness_poller::k_reserved);
                if (c.fence_fd < 0)
                    send_ready(f, f->meta);
                else if (cu_poll_add(c.fence_fd, k_poll_base + so->handle, c.serial))
                    f->state = frame_state::waiting_fence;
                else
                    send_failed(f, capture_failure::unknown);
                break;
            case capture_result::readback:
                f->meta = c;
                f->meta.fence_fd = -1;
                f->has_meta = true;
                f->state = frame_state::waiting_read;
                if (c.fence_fd >= 0 &&
                    !cu_poll_add(c.fence_fd, k_poll_base + so->handle, c.serial)) {
                    // No poller entry: copy out now (the map waits for the read).
                    capture_fence_signalled(k_poll_base + so->handle, c.serial);
                }
                break;
            case capture_result::ready:
                send_ready(f, c.finished_read && f->has_meta ? f->meta : c);
                break;
            }
            if (f->resource)
                wl_client_flush(wl_resource_get_client(f->resource));
        }
        if (n < 32)
            break;
    }
#else
    (void)ctx;
#endif
}

void lx::compositor::capture_tick(p0_protocol_context& ctx, unsigned frame, bool publish) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    using namespace lx::compositor::p0;
    capture_host* h = ctx.capture;
    if (!h)
        return;
    h->frame = frame;
    capture_lock_check(ctx);
    if (g_sessions.count() > 0) {
        g_sessions.for_each([&](unsigned handle, capture_session& cs) {
            session_obj* so = session_of(handle);
            if (!so || cs.stopped)
                return;
            // Constraints follow the source: a new mode, a resized or rescaled window.
            const capture_constraints c = constraints_for(cs, so);
            if (!c.valid()) {
                stop_session(so, "source gone");
                return;
            }
            if (so->screencopy) {
                // One frame, its buffer fixed at creation (render fails it if it no longer
                // fits the output).
                schedule(so, publish);
                return;
            }
            if (!(c == cs.constraints)) {
                cs.constraints = c;
                cs.full_damage = true;
                send_constraints(so, c);
            }
            schedule(so, publish);
        });
    }
    capture_cursor_tick();
    // CI1: a capture by anyone but the shell: an ext session that has delivered a frame, for
    // as long as it stays open, or a wlr-screencopy frame delivered in the last 2 s (each of
    // its frames is a one-shot session). Not "a frame in the last 2 s" for ext sessions: the
    // shell repainting the indicator is damage, so an idle cast would see the dot go, copy
    // that frame, see it come back, copy again — blinking and copying every ~2 s forever.
    bool active = false;
    if (g_sessions.count() > 0) {
        const unsigned long long now = mono_ns();
        wl_client* shell = nullptr;
        if (ctx.shell && ctx.shell->bridge_resource())
            shell = wl_resource_get_client(static_cast<wl_resource*>(ctx.shell->bridge_resource()));
        g_sessions.for_each([&](unsigned handle, capture_session& cs) {
            const session_obj* so = session_of(handle);
            if (so && so->client != shell && !cs.stopped && so->last_ready_ns &&
                (!so->screencopy || now - so->last_ready_ns < k_indicator_ns))
                active = true;
        });
    }
    if (active != g_indicator) {
        g_indicator = active;
        shell_send_capture_state(active);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.capture",
                                        active ? "screen capture active" : "screen capture idle");
    }
#else
    (void)ctx;
    (void)frame;
    (void)publish;
#endif
}

void lx::compositor::capture_lock_check(p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    using namespace lx::compositor::p0;
    capture_host* h = ctx.capture;
    const lock_state now = ctx.lock ? ctx.lock->state() : lock_state::unlocked;
    const bool was_locked = g_lock_seen != lock_state::unlocked;
    const bool locked = now != lock_state::unlocked;
    g_lock_seen = now;
    if (!h || was_locked == locked)
        return;
    if (locked) {
        // From the lock request on (CC7): content jobs already with render are cancelled
        // (they would copy a frame from before the lock) and every session's next frame is the
        // curtain.
        if (h->channel)
            h->channel->lock_epoch.fetch_add(1, std::memory_order_acq_rel);
        g_sessions.for_each([](unsigned, capture_session& cs) { cs.curtained = false; });
    } else {
        // Unlocked: real frames again, the first in full.
        g_sessions.for_each([](unsigned, capture_session& cs) {
            if (cs.curtained)
                cs.full_damage = true;
            cs.curtained = false;
        });
    }
    // Render reads the epoch whenever it next looks at a job; the curtains are posted (and
    // render woken) by the next capture tick.
#else
    (void)ctx;
#endif
}

long long lx::compositor::capture_view_floor(const p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    using namespace lx::compositor::p0;
    if (!ctx.capture || g_toplevel_views == 0)
        return -1;
    long long floor = -1;
    g_sessions.for_each([&](unsigned handle, capture_session& cs) {
        const session_obj* so = session_of(handle);
        if (cs.kind != capture_source_kind::toplevel || !so || !so->frame)
            return;
        const frame_obj* f = so->frame;
        if (f->state != frame_state::posted || !f->view_job)
            return;
        if (floor < 0 || static_cast<long long>(f->view_frame) < floor)
            floor = f->view_frame;
    });
    return floor;
#else
    (void)ctx;
    return -1;
#endif
}

std::uint32_t lx::compositor::capture_wanted_outputs(const p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_IMAGE_CAPTURE)
    using namespace lx::compositor::p0;
    const capture_host* h = ctx.capture;
    if (!h || !h->scene || g_toplevel_views == 0)
        return 0;
    std::uint32_t mask = 0;
    g_sessions.for_each([&](unsigned handle, capture_session& cs) {
        const session_obj* so = session_of(handle);
        if (cs.kind != capture_source_kind::toplevel || !so || !so->frame ||
            so->frame->state != frame_state::requested)
            return;
        const bool due = !cs.delivered || cs.full_damage || so->view < 0 ||
                         !h->scene->pending_damage(lx::scene::k_capture_view_base +
                                                   static_cast<unsigned>(so->view))
                              .empty();
        if (!due)
            return;
        // The window's output paces its view; any live one when it is on none.
        const toplevel_record* rec = window_of(cs, so);
        const int slot = rec && rec->output >= 0 && rec->output < 16 &&
                                 h->outputs[rec->output].live
                             ? rec->output
                             : -1;
        if (slot >= 0) {
            mask |= 1u << static_cast<unsigned>(slot);
        } else {
            for (unsigned s = 0; s < 16; ++s)
                if (h->outputs[s].live) {
                    mask |= 1u << s;
                    break;
                }
        }
    });
    return mask;
#else
    (void)ctx;
    return 0;
#endif
}

#else // !LUMEN_HAS_WAYLAND

void lx::compositor::capture_complete(p0_protocol_context&) {}
void lx::compositor::capture_tick(p0_protocol_context&, unsigned, bool) {}
void lx::compositor::capture_lock_check(p0_protocol_context&) {}
std::uint32_t lx::compositor::capture_wanted_outputs(const p0_protocol_context&) { return 0; }
long long lx::compositor::capture_view_floor(const p0_protocol_context&) { return -1; }

#endif // LUMEN_HAS_WAYLAND
