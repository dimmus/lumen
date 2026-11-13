module;

// xdg-toplevel-drag-v1 (staging): a drag-and-drop operation that moves a window with the
// pointer — a detachable part of a window dragged out becomes a window of its own, or an
// existing window is dragged by attaching it before start_drag. This unit owns the two
// wire objects only; the move itself is the window manager's (`drag_toplevel_attach` /
// `drag_toplevel_detach`), driven through the hooks wire_dnd calls at the drag's start and
// at each of its ends (release-drop, cancel, Escape, source destroyed, client gone).
//
// Choices, against the peers (KWin, Hyprland; sway ships the same semantics):
// - get_xdg_toplevel_drag marks the source (its `toplevel_drag` back-pointer), once per
//   source: a second object for it is invalid_source, and the mark makes a later
//   wl_data_device.set_selection with it a drag-source error ("Attempting to use the source
//   other than for drag-and-drop ... will raise an invalid_source error" — raised as
//   wl_data_source.invalid_source, the error the source interface defines for exactly this,
//   as a source that declared actions already gets in xfer_set_selection).
// - attach works before or after start_drag (the XML's two workflows). The offset is
//   honoured where the XML asks for it, a window that maps mid-drag: it lands with the
//   hotspot at that point of its window geometry. A mapped window keeps its position and
//   follows the pointer's delta, as KWin's toplevelDrag moves it.
// - destroy refuses with ongoing_drag until the drag ended (dnd_drop_performed or
//   cancelled went out); after that it succeeds. A client that disconnects mid-drag is
//   cleaned up by the resource destructor (nothing is posted to a dying client).
// - Destroying the data_source while a toplevel is attached is undefined per the XML; the
//   compositor survives it: dnd_source_destroyed ends the drag, which detaches the window,
//   and the source's mark and the object's back-pointer clear each other when either side
//   goes, so whichever dies first leaves nothing dangling.
// Part of the protocol wire split; shared declarations live in the :wire partition.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_DRAG_V1)
#include "xdg-shell-server-protocol.h"
#include "xdg-toplevel-drag-v1-server-protocol.h"
#define LUMEN_HAS_TOPLEVEL_DRAG 1
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import :toplevel;
import :wm;
import :wire;

#ifdef LUMEN_HAS_TOPLEVEL_DRAG

namespace lx::compositor::wire {

/// xdg_toplevel_drag_v1. One per source at most (a source is usable once), created by
/// get_xdg_toplevel_drag, freed with its resource. The source's `toplevel_drag` mark and
/// the object's `source` clear each other when either side goes, so neither can dangle —
/// a source destroyed while a toplevel is attached is undefined per the XML, survived.
struct toplevel_drag_obj {
    wl_resource* resource = nullptr;
    /// The marked source; null once it went (or the mark was cleared).
    xfer_source* source = nullptr;
    /// The attached toplevel; null when none. Kept after it unmapped or was destroyed:
    /// whether it still blocks a new attach is decided from the record's liveness and
    /// unmap epoch, and ids are never reused.
    lx::toplevel_id toplevel{};
    /// attach's offset: the point of the window geometry the pointer hotspot holds when
    /// the window maps mid-drag.
    lx::point2i offset{};
    /// `unmap_epoch` of the toplevel at the attach: still equal while it never unmapped.
    unsigned epoch = 0;
    bool attached = false;
};

namespace {

constexpr lx::trace::category log_dnd{"compositor.dnd"};

/// The object whose source's drag is running (one drag per seat; null when none runs, or
/// the drag's source has no object). destroy refuses while this is set (ongoing_drag).
toplevel_drag_obj* active_drag = nullptr;

unsigned res_id(wl_resource* r) { return r ? wl_resource_get_id(r) : 0; }

toplevel_drag_obj* drag_of(wl_resource* r) {
    if (!r)
        return nullptr;
    auto* d = static_cast<toplevel_drag_obj*>(wl_resource_get_user_data(r));
    return d && d->resource == r ? d : nullptr;
}

/// The attached toplevel's record, when it still exists (the xdg_toplevel was not
/// destroyed). Unmapped is still attached-as-far-as-the-object-knows: the window manager
/// auto-detached the move; attach decides from `mapped` whether a new one is allowed.
lx::compositor::toplevel_record* attached_record(toplevel_drag_obj* d) {
    if (!d || !d->attached || !d->toplevel || !g_ctx || !g_ctx->toplevels)
        return nullptr;
    return g_ctx->toplevels->find_mut(d->toplevel);
}

/// Hands the attachment to the window manager — only while `d`'s drag runs: before the
/// start the start itself hands it over, after the end the position is final.
void move_with_drag(toplevel_drag_obj* d) {
    if (!g_ctx || !g_ctx->wm || active_drag != d)
        return;
    if (auto* rec = attached_record(d))
        g_ctx->wm->drag_toplevel_attach(rec->id, d->offset);
}

void drag_destroy(wl_client*, wl_resource* r) {
    auto* d = drag_of(r);
    if (d && d == active_drag) {
        // "This request must only be called after the underlying wl_data_source drag has
        // ended ... In any other case an ongoing_drag error is raised."
        wl_resource_post_error(r, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG,
                               "xdg_toplevel_drag_v1@%u destroyed before the drag ended",
                               wl_resource_get_id(r));
        return;
    }
    wl_resource_destroy(r);
}

void drag_attach(wl_client*, wl_resource* r, wl_resource* toplevel_res, int32_t x_offset,
                 int32_t y_offset) {
    auto* d = drag_of(r);
    lx::compositor::toplevel_record* rec = record_of(toplevel_of(toplevel_res));
    // "Issuing it while a toplevel with an active role is attached raises
    // toplevel_attached" (KWin refuses any second attach while its toplevel is set): one
    // that unmapped auto-detached — its unmap epoch moved, and a re-attach is then what
    // the XML asks for — and one that was destroyed took its role with it.
    if (d && d->attached) {
        if (auto* old = attached_record(d); old && old->unmap_epoch == d->epoch) {
            wl_resource_post_error(r, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
                                   "xdg_toplevel_drag_v1@%u already has a toplevel attached",
                                   wl_resource_get_id(r));
            return;
        }
    }
    if (!d || !rec)
        return; // the toplevel was destroyed in the same flush: inert (wire_dialog's rule)
    d->toplevel = rec->id;
    d->offset = {x_offset, y_offset};
    d->epoch = rec->unmap_epoch;
    d->attached = true;
    if (log_dnd.enabled(lx::trace::level::debug)) {
        log_dnd.debug("toplevel-drag attach toplevel={} x={} y={} dragging={}", rec->id.id(),
                      x_offset, y_offset, active_drag == d ? 1 : 0);
    }
    move_with_drag(d);
}

void drag_destroy_resource(wl_resource* r) {
    auto* d = drag_of(r);
    if (!d)
        return;
    if (active_drag == d) {
        // The client is going mid-drag: the move stops where it is; the drag itself ends
        // with the client (its source's destructor runs dnd_source_destroyed).
        active_drag = nullptr;
        if (g_ctx && g_ctx->wm)
            g_ctx->wm->drag_toplevel_detach();
    }
    // The mark goes with the object: the source may be given a new one (KWin's
    // lifecycle) — and a source destroyed first has already nulled `d->source`.
    if (d->source)
        d->source->toplevel_drag = nullptr;
    delete d;
}

const struct xdg_toplevel_drag_v1_interface drag_impl = {
    .destroy = &drag_destroy,
    .attach = &drag_attach,
};

void manager_get_drag(wl_client* client, wl_resource* resource, uint32_t id,
                      wl_resource* source_res) {
    xfer_source* src = xfer_source_from(source_res);
    if (source_res && !src)
        return; // destroyed in the same flush
    if (src && src->toplevel_drag) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
                               "wl_data_source@%u already used for a toplevel drag",
                               res_id(source_res));
        return;
    }
    auto* d = new (std::nothrow) toplevel_drag_obj{};
    wl_resource* res =
        d ? wl_resource_create(client, &xdg_toplevel_drag_v1_interface,
                               wl_resource_get_version(resource), id)
          : nullptr;
    if (!res) {
        delete d;
        wl_client_post_no_memory(client);
        return;
    }
    d->resource = res;
    if (src) {
        d->source = src;
        src->toplevel_drag = d;
    }
    wl_resource_set_implementation(res, &drag_impl, d, drag_destroy_resource);
}

const struct xdg_toplevel_drag_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_xdg_toplevel_drag = &manager_get_drag,
};

/// dnd_start_drag validated the drag: its object, when the source has one, is the drag's
/// from here — destroy refuses (ongoing_drag) and an attached toplevel starts moving.
void started(xfer_source* source) {
    auto* d = source ? source->toplevel_drag : nullptr;
    if (!d)
        return;
    active_drag = d;
    move_with_drag(d);
}

/// The drag ended (dropped, cancelled, the source or its client gone): the attached
/// toplevel keeps its final position.
void ended() {
    if (!active_drag)
        return;
    active_drag = nullptr;
    if (g_ctx && g_ctx->wm)
        g_ctx->wm->drag_toplevel_detach();
}

} // namespace

void bind_toplevel_drag_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_ctx)
        return;
    auto res = wayland::resource::create(client, &xdg_toplevel_drag_manager_v1_interface,
                                         version, id);
    if (res)
        res.set_implementation(&manager_impl, g_ctx, nullptr);
}

} // namespace lx::compositor::wire

#endif // LUMEN_HAS_TOPLEVEL_DRAG

// The drag hooks wire_dnd calls. wire_dnd is always compiled, the unit above only with the
// glue — so these exist unconditionally and forward inside the guard. Without it no source
// is ever marked and no drag can have a toplevel attached; the bodies are empty.
void lx::compositor::wire::toplevel_drag_started(xfer_source* source) {
#ifdef LUMEN_HAS_TOPLEVEL_DRAG
    started(source);
#else
    (void)source;
#endif
}

void lx::compositor::wire::toplevel_drag_ended() {
#ifdef LUMEN_HAS_TOPLEVEL_DRAG
    ended();
#endif
}

void lx::compositor::wire::toplevel_drag_source_destroyed(xfer_source* source) {
#ifdef LUMEN_HAS_TOPLEVEL_DRAG
    if (source && source->toplevel_drag)
        source->toplevel_drag->source = nullptr;
#else
    (void)source;
#endif
}
