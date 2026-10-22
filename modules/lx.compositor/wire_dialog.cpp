module;

// xdg-dialog-v1 (staging): marks an xdg_toplevel as a dialog and, with set_modal, as
// blocking its parent (design-app-compat-capture B.1, D1). The policy — the parent never
// takes the keyboard while it has a modal child, a click on it focuses the dialog, the
// dialog stays above it and is centred on it — is the window manager's (`set_modal`,
// `modal_blocker`); this unit only tracks the objects.
//
// set_modal / unset_modal take effect at once, not with a commit: the protocol defines no
// double-buffering for them, and Mutter applies them in the request
// (meta-wayland-xdg-dialog.c). Destroying the dialog object unsets modal.
//
// Every error:
//   xdg_wm_dialog_v1.already_used  a live xdg_dialog_v1 exists for that xdg_toplevel
// An xdg_dialog_v1 whose xdg_toplevel is gone is inert ("destroying the xdg_toplevel
// before the xdg_dialog_v1 has no effect on it", as wlroots treats it).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_XDG_DIALOG_V1)
#include "xdg-shell-server-protocol.h"
#include "xdg-dialog-v1-server-protocol.h"
#define LUMEN_HAS_XDG_DIALOG 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import :toplevel;
import :wire;

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_XDG_DIALOG)

namespace lx::compositor::p0 {

namespace {

/// xdg_dialog_v1. Keyed by the toplevel id: ids are never reused, so a dialog whose window
/// went finds no record and is inert. Allocated per request, freed with its resource.
struct dialog_obj {
    wl_resource* resource = nullptr;
    lx::toplevel_id toplevel{};
};

lx::compositor::toplevel_record* dialog_record(dialog_obj* d) {
    if (!d || !d->toplevel || !g_p0_ctx || !g_p0_ctx->toplevels)
        return nullptr;
    return g_p0_ctx->toplevels->find_mut(d->toplevel);
}

void dialog_set_modal(wl_resource* r, bool modal) {
    auto* d = static_cast<dialog_obj*>(wl_resource_get_user_data(r));
    if (auto* rec = dialog_record(d); rec && g_p0_ctx->wm)
        g_p0_ctx->wm->set_modal(rec->id, modal);
}

void dialog_destroy_resource(wl_resource* r) {
    auto* d = static_cast<dialog_obj*>(wl_resource_get_user_data(r));
    if (!d)
        return;
    if (auto* rec = dialog_record(d)) {
        rec->dialog = false;
        // Not at client teardown: the window goes with the client anyway, and the shell must
        // not be told a dying window changed state.
        if (g_p0_ctx->wm && !client_dying(wl_resource_get_client(r)))
            g_p0_ctx->wm->set_modal(rec->id, false);
        else
            rec->modal = false;
    }
    delete d;
}

const struct xdg_dialog_v1_interface dialog_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_modal = [](struct wl_client*, struct wl_resource* r) { dialog_set_modal(r, true); },
    .unset_modal = [](struct wl_client*, struct wl_resource* r) { dialog_set_modal(r, false); },
};

void wm_dialog_get(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                   struct wl_resource* toplevel_res) {
    lx::compositor::toplevel_record* rec = record_of(toplevel_of(toplevel_res));
    if (rec && rec->dialog) {
        wl_resource_post_error(resource, XDG_WM_DIALOG_V1_ERROR_ALREADY_USED,
                               "the xdg_toplevel already has an xdg_dialog_v1");
        return;
    }
    auto* d = new (std::nothrow) dialog_obj{};
    wl_resource* res =
        d ? wl_resource_create(client, &xdg_dialog_v1_interface, wl_resource_get_version(resource), id)
          : nullptr;
    if (!res) {
        delete d;
        wl_client_post_no_memory(client);
        return;
    }
    d->resource = res;
    if (rec) {
        d->toplevel = rec->id;
        rec->dialog = true;
    }
    wl_resource_set_implementation(res, &dialog_impl, d, dialog_destroy_resource);
}

const struct xdg_wm_dialog_v1_interface wm_dialog_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_xdg_dialog = &wm_dialog_get,
};

} // namespace

void bind_wm_dialog(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &xdg_wm_dialog_v1_interface, version, id);
    if (res)
        res.set_implementation(&wm_dialog_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND && LUMEN_HAS_XDG_DIALOG
