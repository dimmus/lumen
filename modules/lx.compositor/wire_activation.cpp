module;

// xdg_activation_v1 (xdg-activation): issuing tokens and presenting them. Part of the protocol
// wire split; shared declarations live in the :wire partition (wire.cppm).
//
// The wire owns the token builders (xdg_activation_token_v1) and decides, at commit, whether
// a token may later grant focus — that needs the per-client input serial log and who holds
// the keyboard, both here. What presenting a token does is the window manager's call
// (`window_manager::request_activation`, the policy in :activation). xdg-activation is a P1
// protocol: without its generated header this unit compiles to the two helpers below.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_XDG_ACTIVATION_V1)
#include "xdg-activation-v1-server-protocol.h"
#define LUMEN_HAS_XDG_ACTIVATION 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.runtime;
import lx.trace;
import :surface;
import :surface_tree;
import :toplevel;
import :activation;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

unsigned wm_last_user_serial(void* user, void* surface) {
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (!ctx || !ctx->input || !surface)
        return 0;
    return ctx->input->serials().last_user_serial(
        wl_resource_get_client(static_cast<wl_resource*>(surface)));
}

void activation_surface_destroyed(lx::surface_id surface) {
    if (g_p0_ctx && g_p0_ctx->activation)
        g_p0_ctx->activation->forget_surface(surface);
}

#if defined(LUMEN_HAS_XDG_ACTIVATION)

namespace {

constexpr const char* k_category = "compositor.activation";

[[nodiscard]] bool activation_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

/// The frame ticks' clock (std::chrono::steady_clock): token ages compare with it.
lx::runtime::clock_time steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The first 8 printable characters of a client-supplied token, for a log line.
void log_prefix(const char* token, char (&out)[9]) {
    unsigned n = 0;
    for (; token && token[n] && n < 8; ++n)
        out[n] = (token[n] > 0x20 && token[n] < 0x7f) ? token[n] : '?';
    out[n] = '\0';
    if (n == 0) {
        out[0] = '-';
        out[1] = '\0';
    }
}

/// xdg_activation_token_v1 before and after commit. The token itself lives in the
/// compositor's table; destroying this object leaves it valid (the spec).
///
/// One allocation per get_activation_token, freed with the object (wlroots, KWin): a shared
/// fixed table would let one client that never destroys its builders make the next
/// client's request fail with no_memory, which is fatal to that innocent client. A request
/// per launch or activation, never per frame.
struct token_builder {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    lx::client_id client{};
    unsigned serial = 0;
    bool has_serial = false;
    lx::surface_id surface{};
    char app_id[128]{};
    bool committed = false;
};

token_builder* builder_from(wl_resource* resource) {
    auto* b = static_cast<token_builder*>(wl_resource_get_user_data(resource));
    return b && b->resource == resource ? b : nullptr;
}

// set_serial / set_app_id / set_surface after commit: ignored — "must be sent before commit"
// comes with no error code.

void token_set_serial(wl_client*, wl_resource* resource, uint32_t serial, wl_resource* seat) {
    token_builder* b = builder_from(resource);
    if (!b || b->committed)
        return;
    b->serial = serial;
    b->has_serial = seat != nullptr;
}

void token_set_app_id(wl_client*, wl_resource* resource, const char* app_id) {
    token_builder* b = builder_from(resource);
    if (!b || b->committed)
        return;
    std::snprintf(b->app_id, sizeof(b->app_id), "%s", app_id ? app_id : "");
}

void token_set_surface(wl_client*, wl_resource* resource, wl_resource* surface) {
    token_builder* b = builder_from(resource);
    if (!b || b->committed)
        return;
    const surface_obj* s = surface_from(surface);
    b->surface = s ? s->id : lx::surface_id{};
}

void token_commit(wl_client* client, wl_resource* resource) {
    token_builder* b = builder_from(resource);
    if (!b)
        return;
    if (b->committed) {
        wl_resource_post_error(resource, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
                               "xdg_activation_token_v1 already committed");
        return;
    }
    b->committed = true;
    p0_protocol_context* ctx = b->ctx;
    const lx::runtime::clock_time now = steady_ns();
    activation_token* t = ctx && ctx->activation
                              ? ctx->activation->create(now, k_activation_lifetime_ns)
                              : nullptr;
    if (!t) {
        // No randomness: a token that matches nothing ("the compositor will provide an
        // invalid token").
        xdg_activation_token_v1_send_done(resource, "invalid");
        lx::trace::logger::global().log(lx::trace::level::warn, k_category,
                                        "token create failed: no randomness available");
        return;
    }
    t->grant.issuer = b->client;
    t->grant.serial = b->serial;
    t->grant.has_serial = b->has_serial;
    t->surface = b->surface;
    std::memcpy(t->app_id, b->app_id, sizeof(t->app_id));

    activation_issue issue{};
    issue.has_serial = b->has_serial;
    if (ctx->input) {
        const input_serial_log& log = ctx->input->serials();
        issue.serial_delivered = log.delivered(client, b->serial);
        issue.issuer_has_keyboard = ctx->input->keyboard_focus_client() == client;
        issue.serial_is_last_press =
            b->serial != 0 &&
            (b->serial == log.last_button_press(client) || b->serial == log.last_key_press(client));
    }
    const activation_reason why = decide_grantable(issue);
    t->grant.grantable = why == activation_reason::ok;
    t->grant.refusal = why;
    xdg_activation_token_v1_send_done(resource, t->text);

    if (activation_debug()) {
        lx::trace::logger::global().logf(lx::trace::level::debug, k_category,
                                         "token create client={} token={:.8} serial={} surface={} "
                                         "app_id={} grantable={} reason={}",
                                         b->client.id(), t->text, b->has_serial ? b->serial : 0u,
                                         b->surface.id(), b->app_id[0] ? b->app_id : "-",
                                         t->grant.grantable ? 1 : 0, activation_reason_name(why));
    }
}

void token_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void token_destroy_resource(wl_resource* resource) {
    delete builder_from(resource);
}

const struct xdg_activation_token_v1_interface token_impl = {
    .set_serial = token_set_serial,
    .set_app_id = token_set_app_id,
    .set_surface = token_set_surface,
    .commit = token_commit,
    .destroy = token_destroy,
};

void activation_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void activation_get_token(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* b = new (std::nothrow) token_builder{};
    wl_resource* res = b ? wl_resource_create(client, &xdg_activation_token_v1_interface,
                                              wl_resource_get_version(resource), id)
                         : nullptr;
    if (!res) {
        delete b;
        wl_client_post_no_memory(client);
        return;
    }
    b->resource = res;
    b->ctx = g_p0_ctx;
    b->client = lx::client_id{wire_client_id(client)};
    wl_resource_set_implementation(res, &token_impl, b, token_destroy_resource);
}

/// The toplevel a surface activates: its own, or its root's for a subsurface. Popups, layer
/// and cursor surfaces, and surfaces with no role, have none.
lx::toplevel_id toplevel_for(p0_protocol_context* ctx, surface_obj* s) {
    if (!s)
        return {};
    surface_obj* root = s;
    if (s->role == surface_role::subsurface && ctx && ctx->tree)
        root = surface_by_id(ctx->tree->root_of(s->id));
    return root && root->role == surface_role::xdg_toplevel ? root->toplevel : lx::toplevel_id{};
}

void activation_activate(wl_client*, wl_resource*, const char* token, wl_resource* surface) {
    p0_protocol_context* ctx = g_p0_ctx;
    if (!ctx)
        return;
    const lx::runtime::clock_time now = steady_ns();
    // An unknown token is not an error ("compositors can ignore unknown activation tokens").
    activation_token* t = ctx->activation ? ctx->activation->find(token) : nullptr;
    surface_obj* s = surface_from(surface);
    const lx::toplevel_id target = toplevel_for(ctx, s);
    activation_reason why = activation_reason::not_toplevel;
    activation_result r = activation_result::ignored;
    if (target && ctx->wm)
        r = ctx->wm->request_activation(target, t ? &t->grant : nullptr, now, why);
    if (activation_debug()) {
        char prefix[9];
        log_prefix(token, prefix);
        lx::trace::logger::global().logf(lx::trace::level::debug, k_category,
                                         "activate token={} surface={} id={} result={} reason={}",
                                         prefix, s ? s->id.id() : 0ull, target.id(),
                                         activation_result_name(r), activation_reason_name(why));
    }
    // Single use (KWin, Mutter, niri): presenting it for a window spends it, whatever the
    // outcome. Presented for no window, it stays.
    if (t && target)
        ctx->activation->erase(t);
}

const struct xdg_activation_v1_interface activation_impl = {
    .destroy = activation_destroy,
    .get_activation_token = activation_get_token,
    .activate = activation_activate,
};

} // namespace

void bind_activation(wayland::client_connection& client, unsigned id, int version) {
    auto res = wayland::resource::create(client, &xdg_activation_v1_interface, version, id);
    if (!res)
        return;
    wl_resource_set_implementation(static_cast<wl_resource*>(res.native()), &activation_impl,
                                   nullptr, nullptr);
}

#endif // LUMEN_HAS_XDG_ACTIVATION

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
