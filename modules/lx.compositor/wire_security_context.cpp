module;

// security-context-v1 (staging): a sandbox engine (Flatpak 1.16 and later) hands the
// compositor a listening socket and metadata (engine, app id, instance id); every client
// that connects through that socket carries the context and is sandboxed
// (design-app-compat-capture B.7, SC1). The server accepts on it (`server::add_security_
// listener`, sources on libwayland's own loop) and the existing global filter (ADR 008 C5)
// then hides every session-wide global from those clients — exactly, where the older
// `/.flatpak-info` heuristic (kept for older Flatpak) had to guess.
//
// The manager itself is hidden from sandboxed clients (no nesting); a client with a context
// that binds it anyway (it cannot: the bind of a filtered global is refused) would get
// `nested`.
//
// Every error:
//   wp_security_context_manager_v1.invalid_listen_fd  not a listening AF_UNIX stream socket
//   wp_security_context_manager_v1.nested             the creating client has a context
//   wp_security_context_v1.already_used               any request but destroy after commit
//   wp_security_context_v1.already_set                a second set_sandbox_engine /
//                                                     set_app_id / set_instance_id
//   wp_security_context_v1.invalid_metadata           an empty, overlong (> 255 bytes) or
//                                                     non-printable string
// The fds are owned from create_listener on and closed in every path that does not hand them
// to the server.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#include <sys/socket.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_SECURITY_CONTEXT_V1)
#include "security-context-v1-server-protocol.h"
#define LUMEN_HAS_SECURITY_CONTEXT 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :wire;

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_SECURITY_CONTEXT)

namespace lx::compositor::p0 {

namespace {

/// wp_security_context_v1 before (and after) commit. Allocated per request.
struct context_obj {
    wl_resource* resource = nullptr;
    int listen_fd = -1;
    int close_fd = -1;
    unsigned creator = 0;
    wayland::security_context ctx{};
    bool engine_set = false;
    bool app_id_set = false;
    bool instance_set = false;
    bool committed = false;
};

void close_fds(context_obj* c) {
    if (c->listen_fd >= 0)
        ::close(c->listen_fd);
    if (c->close_fd >= 0)
        ::close(c->close_fd);
    c->listen_fd = c->close_fd = -1;
}

void context_destroy_resource(wl_resource* r) {
    auto* c = static_cast<context_obj*>(wl_resource_get_user_data(r));
    if (!c)
        return;
    close_fds(c);
    delete c;
}

/// A listening AF_UNIX stream socket (wlroots' check).
bool valid_listen_fd(int fd) {
    int value = 0;
    socklen_t len = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &len) != 0 || !value)
        return false;
    len = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &value, &len) != 0 || value != AF_UNIX)
        return false;
    len = sizeof(value);
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &len) == 0 && value == SOCK_STREAM;
}

/// Printable ASCII, 1…255 bytes: what an engine name, app id or instance id is.
bool valid_metadata(const char* v) {
    if (!v || !v[0])
        return false;
    std::size_t n = 0;
    for (const char* p = v; *p; ++p, ++n) {
        const auto ch = static_cast<unsigned char>(*p);
        if (ch < 0x20 || ch == 0x7f || n >= wayland::security_context::k_bytes - 1)
            return false;
    }
    return true;
}

/// The common checks of the three setters; false when an error was posted.
bool set_field(wl_resource* r, bool& set, char (&field)[wayland::security_context::k_bytes],
               const char* value) {
    auto* c = static_cast<context_obj*>(wl_resource_get_user_data(r));
    if (!c)
        return false;
    if (c->committed) {
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
                               "the security context was already committed");
        return false;
    }
    if (set) {
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
                               "this metadata was already set");
        return false;
    }
    if (!valid_metadata(value)) {
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_INVALID_METADATA,
                               "empty, overlong or non-printable metadata");
        return false;
    }
    std::snprintf(field, sizeof(field), "%s", value);
    set = true;
    return true;
}

context_obj* ctx_of(wl_resource* r) { return static_cast<context_obj*>(wl_resource_get_user_data(r)); }

const struct wp_security_context_v1_interface context_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_sandbox_engine =
        [](struct wl_client*, struct wl_resource* r, const char* name) {
            if (auto* c = ctx_of(r))
                (void)set_field(r, c->engine_set, c->ctx.sandbox_engine, name);
        },
    .set_app_id =
        [](struct wl_client*, struct wl_resource* r, const char* app_id) {
            if (auto* c = ctx_of(r))
                (void)set_field(r, c->app_id_set, c->ctx.app_id, app_id);
        },
    .set_instance_id =
        [](struct wl_client*, struct wl_resource* r, const char* instance_id) {
            if (auto* c = ctx_of(r))
                (void)set_field(r, c->instance_set, c->ctx.instance_id, instance_id);
        },
    .commit =
        [](struct wl_client*, struct wl_resource* r) {
            auto* c = ctx_of(r);
            if (!c)
                return;
            if (c->committed) {
                wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
                                       "the security context was already committed");
                return;
            }
            c->committed = true;
            const int listen_fd = c->listen_fd;
            const int close_fd = c->close_fd;
            c->listen_fd = c->close_fd = -1; // the server owns them now, whatever happens
            auto added =
                g_p0_ctx && g_p0_ctx->server
                    ? g_p0_ctx->server->add_security_listener(listen_fd, close_fd, c->ctx,
                                                              c->creator)
                    : lx::result<void>{lx::make_error(lx::wayland_err::bind_failed, "no server")};
            if (!added) {
                // No protocol error exists for it: connections on that socket are never
                // accepted, and the engine sees them fail.
                lx::trace::logger::global().log_error(added.get_error(), "wayland");
                return;
            }
            lx::trace::logger::global().logf(lx::trace::level::info, "wayland",
                                             "security context listener: engine={} app_id={} "
                                             "instance={}",
                                             c->ctx.sandbox_engine[0] ? c->ctx.sandbox_engine : "-",
                                             c->ctx.app_id[0] ? c->ctx.app_id : "-",
                                             c->ctx.instance_id[0] ? c->ctx.instance_id : "-");
        },
};

void manager_create_listener(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                             int32_t listen_fd, int32_t close_fd) {
    const auto* conn = g_p0_ctx && g_p0_ctx->server ? g_p0_ctx->server->find_client_by_native(client)
                                                    : nullptr;
    if (conn && conn->context()) {
        ::close(listen_fd);
        ::close(close_fd);
        wl_resource_post_error(resource, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_NESTED,
                               "nested security contexts are forbidden");
        return;
    }
    if (!valid_listen_fd(listen_fd)) {
        ::close(listen_fd);
        ::close(close_fd);
        wl_resource_post_error(resource, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD,
                               "listen_fd is not a listening AF_UNIX stream socket");
        return;
    }
    auto* c = new (std::nothrow) context_obj{};
    wl_resource* res = c ? wl_resource_create(client, &wp_security_context_v1_interface,
                                              wl_resource_get_version(resource), id)
                         : nullptr;
    if (!res) {
        delete c;
        ::close(listen_fd);
        ::close(close_fd);
        wl_client_post_no_memory(client);
        return;
    }
    c->resource = res;
    c->listen_fd = listen_fd;
    c->close_fd = close_fd;
    c->creator = conn ? conn->id() : 0;
    wl_resource_set_implementation(res, &context_impl, c, context_destroy_resource);
}

const struct wp_security_context_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .create_listener = &manager_create_listener,
};

} // namespace

void bind_security_context_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_security_context_manager_v1_interface,
                                         version, id);
    if (res)
        res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND && LUMEN_HAS_SECURITY_CONTEXT
