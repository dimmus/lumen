module;

// ext_session_lock_manager_v1 (ext-session-lock-v1.xml, staging, v1): the lock objects, their
// lock surfaces and every error of the protocol. Part of the protocol wire split; shared
// declarations live in the :wire partition (wire.cppm), the state machine in :session_lock
// (pure, tested on its own), and what "locked" means — the hidden scene, the input gate,
// logind's LockedHint, the shell — in the compositor, which executes the machine's actions.
//
// Every error of the protocol:
//   ext_session_lock_v1.role                  get_lock_surface on a surface with another role
//   ext_session_lock_v1.duplicate_output      a second lock surface for an output, per lock
//   ext_session_lock_v1.already_constructed   the surface has a buffer attached or committed
//   ext_session_lock_v1.invalid_destroy       destroy after `locked` (unlock_and_destroy it)
//   ext_session_lock_v1.invalid_unlock        unlock_and_destroy before `locked`
//   ext_session_lock_surface_v1.commit_before_first_ack   a commit before any ack
//   ext_session_lock_surface_v1.null_buffer               a commit without a buffer
//   ext_session_lock_surface_v1.dimensions_mismatch       a size other than the acked one
//   ext_session_lock_surface_v1.invalid_serial            an ack of a serial not outstanding
//                                                          (unknown, already acked, or older)
// The commit-time three are checked on the state the commit produces, before any state
// moves (wlroots checks them at client commit too).
//
// A lock object that is not in charge (another locker holds the lock — it was sent
// `finished` — or it was finished by logind's Unlock) creates inert lock surfaces: no role,
// no configure, nothing drawn. Lock surfaces of the lock in charge are configured at once
// to their output's logical size and again when it changes; the surface maps into the lock
// layer at the output's origin with its first buffer.
//
// Objects are allocated per request and freed with their resource. Exposed to every client
// (sway, niri): same-UID clients are not isolated from each other until sandboxing exists
// (design X1, the future security-context filter).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <new>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if __has_include("ext-session-lock-v1-server-protocol.h")
#include "ext-session-lock-v1-server-protocol.h"
#define LUMEN_HAS_SESSION_LOCK 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import lx.scene;
import lx.input;
import :surface;
import :surface_tree;
import :output;
import :viewport;
import :session_lock;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

[[nodiscard]] unsigned long long monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<unsigned long long>(ts.tv_nsec);
}

[[nodiscard]] bool lock_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void lock_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, "compositor.lock", line);
}

[[nodiscard]] lx::compositor::session_lock* machine() {
    return g_p0_ctx ? g_p0_ctx->lock : nullptr;
}

void apply(const lx::compositor::lock_actions& a) {
    if (!a.empty() && g_p0_ctx && g_p0_ctx->lock_apply)
        g_p0_ctx->lock_apply(g_p0_ctx->lock_user, a);
}

/// A configure serial from the seat's serial space (wm_next_serial's rule).
[[nodiscard]] unsigned next_serial() {
    if (g_p0_ctx && g_p0_ctx->seat)
        return g_p0_ctx->seat->next_serial();
    if (g_p0_ctx && g_p0_ctx->server && g_p0_ctx->server->native_display())
        return wl_display_next_serial(static_cast<wl_display*>(g_p0_ctx->server->native_display()));
    return 1;
}

/// Live output slots, a bit each.
[[nodiscard]] std::uint32_t live_outputs() {
    auto* om = g_p0_ctx ? g_p0_ctx->outputs : nullptr;
    std::uint32_t mask = 0;
    if (!om)
        return mask;
    for (unsigned slot = 0; slot < lx::compositor::output_manager::k_max && slot < 32; ++slot)
        if (om->slot_live(slot))
            mask |= 1u << slot;
    return mask;
}

/// The output's logical rect, or empty when the slot is not live.
[[nodiscard]] lx::rect2i output_rect(unsigned slot) {
    auto* om = g_p0_ctx ? g_p0_ctx->outputs : nullptr;
    if (!om || !om->slot_live(slot))
        return {};
    return om->slot_state(slot).logical_rect();
}

/// ext_session_lock_v1. Allocated per request, freed with its resource.
struct lock_obj {
    wl_resource* resource = nullptr;
    bool locked_sent = false;
    bool finished_sent = false;
    /// Output slots this object made a lock surface for (duplicate_output, per lock).
    std::uint32_t outputs_used = 0;
};

} // namespace

#if defined(LUMEN_HAS_SESSION_LOCK)

namespace {

lock_obj* lock_from(wl_resource* r) {
    return r ? static_cast<lock_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

lock_surface_obj* lock_surface_from(wl_resource* r) {
    return r ? static_cast<lock_surface_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

void send_configure(lock_surface_obj* o, lx::size2i size) {
    auto* m = machine();
    if (!o || !o->record || !m || !g_p0_ctx)
        return;
    const unsigned serial = next_serial();
    m->note_configure(o->record, serial, size);
    o->sent = size;
    ext_session_lock_surface_v1_send_configure(o->resource, serial,
                                               static_cast<uint32_t>(size.width > 0 ? size.width : 0),
                                               static_cast<uint32_t>(size.height > 0 ? size.height : 0));
    if (lock_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "lock surface configure surface=%llu output=%u %dx%d serial=%u",
                      o->surface ? o->surface->id.id() : 0ull, o->output, size.width, size.height,
                      serial);
        lock_log(lx::trace::level::debug, line);
    }
}

/// The record goes (the machine already forgot it, or is told now): the tree leaves the
/// lock layer, the object goes inert, the surface keeps its role (a new lock surface may
/// take it, as a layer surface may).
void make_inert(lock_surface_obj* o, bool tell_machine) {
    if (!o || !o->record)
        return;
    const unsigned record = o->record;
    o->record = 0;
    surface_obj* s = o->surface;
    if (s && g_p0_ctx && g_p0_ctx->tree && g_p0_ctx->tree->root_mapped(s->id))
        g_p0_ctx->tree->unmap_root(s->id);
    if (tell_machine) {
        if (auto* m = machine())
            apply(m->remove_surface(record));
    }
}

void lock_surface_destroy_resource(wl_resource* r) {
    lock_surface_obj* o = lock_surface_from(r);
    if (!o)
        return;
    // "If a lock surface on an active output is destroyed before ... unlock_and_destroy, the
    // compositor must fall back to rendering a solid color": its tree leaves the lock layer,
    // and the keyboard moves to another lock surface.
    make_inert(o, true);
    if (o->surface && o->surface->lock_surface == o)
        o->surface->lock_surface = nullptr;
    delete o;
}

const struct ext_session_lock_surface_v1_interface lock_surface_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .ack_configure =
        [](struct wl_client*, struct wl_resource* r, uint32_t serial) {
            lock_surface_obj* o = lock_surface_from(r);
            auto* m = machine();
            if (!o || !o->record || !m)
                return; // inert: nothing was configured
            if (m->ack(o->record, serial) ==
                lx::compositor::session_lock::ack_result::invalid_serial)
                wl_resource_post_error(r, EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
                                       "ack_configure serial %u was not sent, or is already "
                                       "acked or older than the last acked",
                                       serial);
        },
};

void lock_destroy_resource(wl_resource* r) {
    lock_obj* o = lock_from(r);
    if (!o)
        return;
    // The destroy request (the locker gave up before `locked`, or was finished) or the
    // client's teardown: a locker that dies keeps the session locked (abandoned).
    if (auto* m = machine())
        apply(m->released(r, client_dying(wl_resource_get_client(r))));
    delete o;
}

void lock_get_lock_surface(struct wl_client* client, struct wl_resource* lock_res, uint32_t id,
                           struct wl_resource* surface_res, struct wl_resource* output_res) {
    // Always created, to stay in sync with the client — an inert one when nothing can be
    // shown for it.
    wl_resource* res = wl_resource_create(client, &ext_session_lock_surface_v1_interface,
                                          wl_resource_get_version(lock_res), id);
    auto* o = res ? new (std::nothrow) lock_surface_obj{} : nullptr;
    if (!o) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    o->resource = res;
    wl_resource_set_implementation(res, &lock_surface_impl, o, &lock_surface_destroy_resource);

    lock_obj* lo = lock_from(lock_res);
    surface_obj* s = surface_from(surface_res);
    if (!lo || !s)
        return;
    // The protocol's order: the role, the output, the buffer.
    if ((s->role != surface_role::none && s->role != surface_role::lock) || s->lock_surface) {
        wl_resource_post_error(lock_res, EXT_SESSION_LOCK_V1_ERROR_ROLE,
                               "wl_surface@%u already has another role",
                               wl_resource_get_id(surface_res));
        return;
    }
    const int slot = output_resource_slot(output_res);
    const std::uint32_t bit = slot >= 0 && slot < 32 ? 1u << slot : 0u;
    if (bit && (lo->outputs_used & bit)) {
        wl_resource_post_error(lock_res, EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
                               "a lock surface for this output exists already");
        return;
    }
    if (s->has_content || (s->pending.buffer_set && s->pending.buffer) ||
        (s->has_cached && s->cached.buffer_set && s->cached.buffer)) {
        wl_resource_post_error(lock_res, EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
                               "wl_surface@%u has a buffer attached or committed",
                               wl_resource_get_id(surface_res));
        return;
    }
    auto* m = machine();
    // Inert: this lock is not in charge (finished), or the output is gone.
    if (!m || m->current() != lock_res || lo->finished_sent || !bit)
        return;
    if (!g_p0_ctx->tree || !g_p0_ctx->tree->make_root(s->id, lx::compositor::root_kind::lock)) {
        wl_client_post_no_memory(client);
        return;
    }
    const unsigned record = m->add_surface(res, s->resource, s->id, static_cast<unsigned>(slot));
    if (!record) {
        wl_client_post_no_memory(client);
        return;
    }
    lo->outputs_used |= bit;
    o->surface = s;
    o->record = record;
    o->output = static_cast<unsigned>(slot);
    s->lock_surface = o;
    s->role = surface_role::lock;
    const lx::rect2i r = output_rect(o->output);
    o->origin = {r.x, r.y};
    g_p0_ctx->tree->set_root_origin(s->id, o->origin);
    // "On binding this interface the compositor will immediately send the first configure."
    send_configure(o, {r.width, r.height});
    {
        char line[128];
        std::snprintf(line, sizeof(line), "lock surface create surface=%llu output=%u %dx%d",
                      s->id.id(), o->output, r.width, r.height);
        lock_log(lx::trace::level::info, line);
    }
}

const struct ext_session_lock_v1_interface lock_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* r) {
            // "It is a protocol error to make this request if the locked event was sent" —
            // unless `finished` came after it (the lock is no longer in charge).
            if (lock_obj* o = lock_from(r); o && o->locked_sent && !o->finished_sent) {
                wl_resource_post_error(r, EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY,
                                       "destroy after locked: use unlock_and_destroy");
                return;
            }
            wl_resource_destroy(r);
        },
    .get_lock_surface = &lock_get_lock_surface,
    .unlock_and_destroy =
        [](struct wl_client*, struct wl_resource* r) {
            lock_obj* o = lock_from(r);
            if (!o || !o->locked_sent) {
                wl_resource_post_error(r, EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
                                       "unlock_and_destroy before locked: use destroy");
                return;
            }
            if (auto* m = machine())
                apply(m->unlock(r));
            wl_resource_destroy(r);
        },
};

void manager_lock(struct wl_client* client, struct wl_resource* manager, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &ext_session_lock_v1_interface,
                                          wl_resource_get_version(manager), id);
    auto* o = res ? new (std::nothrow) lock_obj{} : nullptr;
    if (!o) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    o->resource = res;
    wl_resource_set_implementation(res, &lock_impl, o, &lock_destroy_resource);
    if (auto* m = machine()) {
        // "Every output" is the outputs live now (later changes arrive through
        // session_lock_outputs_changed).
        apply(m->set_outputs(live_outputs()));
        apply(m->request(res, monotonic_ns()));
    } else {
        o->finished_sent = true;
        ext_session_lock_v1_send_finished(res);
    }
}

const struct ext_session_lock_manager_v1_interface manager_impl = {
    // Lock objects created from it live on.
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .lock = &manager_lock,
};

} // namespace

void bind_session_lock_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &ext_session_lock_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

bool lock_surface_precheck(surface_obj* s) {
    lock_surface_obj* o = s->lock_surface;
    auto* m = machine();
    if (!o || !o->record || !m)
        return true; // inert: its buffers are released undrawn
    const lx::compositor::surface_geometry_in in = pending_geometry(s);
    const bool has_buffer = in.buffer_w > 0 && in.buffer_h > 0;
    const lx::size2i size = has_buffer ? lx::compositor::compute_geometry(in).size : lx::size2i{};
    using err = lx::compositor::session_lock::commit_error;
    switch (m->check_commit(o->record, has_buffer, size)) {
    case err::none:
        return true;
    case err::before_first_ack:
        wl_resource_post_error(o->resource, EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK,
                               "lock surface committed before the first ack_configure");
        return false;
    case err::null_buffer:
        wl_resource_post_error(o->resource, EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
                               "lock surface committed without a buffer");
        return false;
    case err::dimensions_mismatch:
        wl_resource_post_error(o->resource, EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH,
                               "lock surface %dx%d does not match the acked configure",
                               size.width, size.height);
        return false;
    }
    return true;
}

bool lock_surface_drawable(const surface_obj* s) {
    return s->lock_surface && s->lock_surface->record != 0;
}

void lock_surface_committed(surface_obj* s) {
    lock_surface_obj* o = s->lock_surface;
    auto* m = machine();
    if (!o || !o->record || !m || !s->has_content || !g_p0_ctx)
        return;
    auto* tree = g_p0_ctx->tree;
    if (tree && !tree->root_mapped(s->id) && g_p0_ctx->lock_layer) {
        // Placed first (the tree only records the origin while unmapped), so mapping creates
        // every node where it belongs; the tree sync that follows draws it.
        tree->set_root_origin(s->id, o->origin);
        if (!tree->map_root(s->id, *g_p0_ctx->lock_layer, ~0u)) {
            lock_log(lx::trace::level::warn, "lock surface map failed: lock layer full");
            return;
        }
        char line[96];
        std::snprintf(line, sizeof(line), "lock surface map surface=%llu output=%u",
                      s->id.id(), o->output);
        lock_log(lx::trace::level::info, line);
    }
    apply(m->content(o->record));
}

void lock_surface_surface_destroyed(surface_obj* s) {
    lock_surface_obj* o = s->lock_surface;
    s->lock_surface = nullptr;
    if (!o)
        return;
    make_inert(o, true);
    o->surface = nullptr;
}

void session_lock_outputs_changed() {
    auto* m = machine();
    auto* om = g_p0_ctx ? g_p0_ctx->outputs : nullptr;
    if (!m || !om)
        return;
    const std::uint32_t mask = live_outputs();
    // A lock surface of an output that went is dropped (ADR 008 §B.5): its tree leaves the
    // lock layer and the lock forgets it, and the lock in charge may make one for a new
    // output in that slot later (no duplicate_output). Collected first: dropping one
    // changes the machine's list.
    lock_surface_obj* gone[lx::compositor::output_manager::k_max * 2]{};
    unsigned gone_count = 0;
    m->for_each_surface([&](const lx::compositor::lock_surface_record& r) {
        auto* o = lock_surface_from(static_cast<wl_resource*>(r.resource));
        if (o && o->record && !(mask & (1u << o->output)) &&
            gone_count < sizeof(gone) / sizeof(gone[0]))
            gone[gone_count++] = o;
    });
    for (unsigned i = 0; i < gone_count; ++i) {
        lock_surface_obj* o = gone[i];
        if (lock_obj* lo = lock_from(static_cast<wl_resource*>(m->current())))
            lo->outputs_used &= ~(1u << o->output);
        char line[96];
        std::snprintf(line, sizeof(line), "lock surface dropped surface=%llu output=%u (output gone)",
                      o->surface ? o->surface->id.id() : 0ull, o->output);
        lock_log(lx::trace::level::info, line);
        make_inert(o, true);
    }
    // Every lock surface of a live output follows its size (a new configure) and origin.
    m->for_each_surface([&](const lx::compositor::lock_surface_record& r) {
        auto* o = lock_surface_from(static_cast<wl_resource*>(r.resource));
        if (!o || !o->record || !(mask & (1u << o->output)))
            return;
        const lx::rect2i rect = output_rect(o->output);
        if (o->origin.x != rect.x || o->origin.y != rect.y) {
            o->origin = {rect.x, rect.y};
            if (o->surface && g_p0_ctx->tree) {
                g_p0_ctx->tree->set_root_origin(o->surface->id, o->origin);
                g_p0_ctx->tree->sync_scene(o->surface->id, "output");
            }
        }
        if (o->sent.width != rect.width || o->sent.height != rect.height)
            send_configure(o, {rect.width, rect.height});
    });
    apply(m->set_outputs(mask));
}

bool session_lock_hit_impl(double x, double y, void*& surface_res, lx::surface_id& surface,
                           lx::surface_id& root, double& sx, double& sy) {
    auto* m = machine();
    auto* tree = g_p0_ctx ? g_p0_ctx->tree : nullptr;
    if (!m || !tree || !g_p0_ctx->surfaces || !m->scene_locked())
        return false;
    bool found = false;
    m->for_each_surface([&](const lx::compositor::lock_surface_record& r) {
        if (found || !r.has_content || !tree->root_mapped(r.surface))
            return;
        lx::compositor::tree_hit th{};
        if (!tree->hit(r.surface, x, y, th))
            return;
        surface_res = g_p0_ctx->surfaces->resource(th.surface);
        if (!surface_res)
            return;
        surface = th.surface;
        root = r.surface;
        sx = th.sx;
        sy = th.sy;
        found = true;
    });
    return found;
}

void session_lock_send_impl(const lx::compositor::lock_actions& a) {
    if (a.has(lx::compositor::lock_actions::send_finished) && a.finished) {
        auto* r = static_cast<wl_resource*>(a.finished);
        lock_obj* o = lock_from(r);
        if (o && !o->finished_sent && !client_dying(wl_resource_get_client(r))) {
            o->finished_sent = true;
            ext_session_lock_v1_send_finished(r);
            lock_log(lx::trace::level::info, "session lock: finished sent");
        }
    }
    if (a.has(lx::compositor::lock_actions::send_locked) && a.lock) {
        auto* r = static_cast<wl_resource*>(a.lock);
        lock_obj* o = lock_from(r);
        if (o && !o->locked_sent && !o->finished_sent &&
            !client_dying(wl_resource_get_client(r))) {
            o->locked_sent = true;
            ext_session_lock_v1_send_locked(r);
        }
    }
    if (a.has(lx::compositor::lock_actions::drop_surfaces)) {
        auto* m = machine();
        if (!m)
            return;
        // Every lock surface of the lock that ended: out of the lock layer, inert.
        m->for_each_surface([](const lx::compositor::lock_surface_record& r) {
            make_inert(lock_surface_from(static_cast<wl_resource*>(r.resource)), false);
        });
        m->clear_surfaces();
    }
}

#else // !LUMEN_HAS_SESSION_LOCK

bool lock_surface_precheck(surface_obj*) { return true; }
bool lock_surface_drawable(const surface_obj*) { return false; }
void lock_surface_committed(surface_obj*) {}
void lock_surface_surface_destroyed(surface_obj* s) { s->lock_surface = nullptr; }
void session_lock_outputs_changed() {}
bool session_lock_hit_impl(double, double, void*&, lx::surface_id&, lx::surface_id&, double&,
                           double&) {
    return false;
}
void session_lock_send_impl(const lx::compositor::lock_actions&) {}

#endif // LUMEN_HAS_SESSION_LOCK

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

void lx::compositor::session_lock_send(p0_protocol_context& ctx, const lock_actions& a) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    p0::session_lock_send_impl(a);
#else
    (void)a;
#endif
}

bool lx::compositor::session_lock_hit(p0_protocol_context& ctx, double x, double y,
                                      void*& surface_res, lx::surface_id& surface,
                                      lx::surface_id& root, double& sx, double& sy) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::session_lock_hit_impl(x, y, surface_res, surface, root, sx, sy);
#else
    (void)x;
    (void)y;
    (void)surface_res;
    (void)surface;
    (void)root;
    (void)sx;
    (void)sy;
    return false;
#endif
}
