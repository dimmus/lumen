module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.wayland.protocols;

export module lx.wayland.server;

export namespace lx::wayland {

struct unix_credentials {
    unsigned pid = 0;
    unsigned uid = 0;
    unsigned gid = 0;
};

class client_connection;
class resource;
class server;

/// Opaque bind callback: client, object id, interface version.
using global_bind_fn = void (*)(client_connection& client, unsigned id, int version);

class client_connection {
public:
    [[nodiscard]] unsigned id() const;
    [[nodiscard]] unix_credentials credentials() const;
    [[nodiscard]] bool is_privileged_shell() const;

    /// Native `wl_client*` (nullptr when LUMEN_HAS_WAYLAND is off).
    [[nodiscard]] void* native() const;
    [[nodiscard]] server* owning_server() const;

private:
    friend class server;
    void set_id(unsigned id);
    void set_credentials(unix_credentials cred);
    void set_native(void* wl_client);
    void set_server(server* s);
    void set_privileged_shell(bool privileged);

    unsigned id_ = 0;
    unix_credentials cred_{};
    void* native_ = nullptr; // wl_client*
    server* server_ = nullptr;
    bool privileged_shell_ = false;
};

class resource {
public:
    resource() = default;

    /// Create a `wl_resource` for `interface_desc` (`const wl_interface*`).
    [[nodiscard]] static resource create(client_connection& client, const void* interface_desc,
                                         int version, unsigned id);

    [[nodiscard]] unsigned id() const;
    [[nodiscard]] const char* interface_name() const;
    [[nodiscard]] void* user_data() const;
    void set_user_data(void* data);

    /// `impl` is the generated request vtable (e.g. `struct wl_compositor_interface`).
    void set_implementation(const void* impl, void* data,
                            void (*destroy)(void* wl_resource) = nullptr);

    void post_error(unsigned code, const char* message);

    /// Native `wl_resource*` (nullptr if unset).
    [[nodiscard]] void* native() const;
    [[nodiscard]] explicit operator bool() const { return native_ != nullptr; }

private:
    void* native_ = nullptr; // wl_resource*
    unsigned id_ = 0;
    const char* iface_ = "";
    void* user_data_ = nullptr;
};

struct global_descriptor {
    const char* interface_name = "";
    int version = 1;
    bool privileged = false;
    /// `const wl_interface*` — required for real `wl_global_create`.
    const void* interface_desc = nullptr;
};

class server {
public:
    server();
    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    [[nodiscard]] lx::result<void> bind(const char* socket_name);

    /// Creates the display without listening on a socket. Clients are attached directly to
    /// a connected fd (`wl_client_create`), which is what tests and the dispatch fuzzer
    /// want: no name to collide on, nothing left in `$XDG_RUNTIME_DIR`, and no dependency
    /// on the filesystem at all.
    [[nodiscard]] lx::result<void> bind_socketless();
    [[nodiscard]] int dispatch(int timeout_ms);
    void flush();

    void add_global(global_descriptor desc, global_bind_fn bind_fn);
    [[nodiscard]] bool allow_privileged_global(const unix_credentials& cred,
                                               const char* global_name) const;

    void set_shell_binary_path(const char* path);

    [[nodiscard]] client_connection* find_client(unsigned id);
    [[nodiscard]] client_connection* find_client_by_native(void* wl_client);
    [[nodiscard]] client_connection* ensure_client(void* wl_client);

    /// Native `wl_display*` (nullptr when unavailable).
    [[nodiscard]] void* native_display() const;
    /// Wayland event-loop fd for `lx.runtime::event_loop` integration.
    [[nodiscard]] int event_fd() const;

    [[nodiscard]] bool is_bound() const { return bound_; }
    [[nodiscard]] unsigned global_count() const { return global_count_; }

private:
    static constexpr unsigned k_max_clients = 64;
    static constexpr unsigned k_max_globals = 96;

    struct global_entry {
        global_descriptor desc{};
        global_bind_fn bind_fn = nullptr;
        void* wl_global = nullptr; // wl_global*
        server* owner = nullptr;
        bool used = false;
    };

    [[nodiscard]] lx::result<void> bind_raw_socket(const char* socket_name);
#if defined(LUMEN_HAS_WAYLAND)
    [[nodiscard]] lx::result<void> bind_libwayland(const char* socket_name);
    static void global_bind_trampoline(struct wl_client* client, void* data, uint32_t version,
                                       uint32_t id);
#endif
    void close_listen();
    void remove_client_slot(unsigned index);
    void advertise_pending_globals();

    client_connection clients_[k_max_clients]{};
    unsigned client_count_ = 0;
    unsigned next_client_id_ = 1;
    global_entry globals_[k_max_globals]{};
    unsigned global_count_ = 0;

    int listen_fd_ = -1;
    char socket_path_[108]{};
    bool bound_ = false;
    const char* shell_binary_path_ = "lumen-shell";

#if defined(LUMEN_HAS_WAYLAND)
    wl_display* display_ = nullptr;

    struct client_destroy_link {
        wl_listener listener{};
        server* server = nullptr;
        unsigned client_id = 0;
    };
    client_destroy_link destroy_links_[k_max_clients]{};
#endif
};

} // namespace lx::wayland

module :private;

unsigned lx::wayland::client_connection::id() const { return id_; }
lx::wayland::unix_credentials lx::wayland::client_connection::credentials() const { return cred_; }
bool lx::wayland::client_connection::is_privileged_shell() const { return privileged_shell_; }
void* lx::wayland::client_connection::native() const { return native_; }
lx::wayland::server* lx::wayland::client_connection::owning_server() const { return server_; }
void lx::wayland::client_connection::set_id(unsigned id) { id_ = id; }
void lx::wayland::client_connection::set_credentials(unix_credentials cred) { cred_ = cred; }
void lx::wayland::client_connection::set_native(void* wl_client) { native_ = wl_client; }
void lx::wayland::client_connection::set_server(server* s) { server_ = s; }
void lx::wayland::client_connection::set_privileged_shell(bool privileged) {
    privileged_shell_ = privileged;
}

lx::wayland::resource lx::wayland::resource::create(client_connection& client,
                                                    const void* interface_desc, int version,
                                                    unsigned id) {
    resource out{};
#if defined(LUMEN_HAS_WAYLAND)
    auto* wl_client = static_cast<struct wl_client*>(client.native());
    auto* iface = static_cast<const struct wl_interface*>(interface_desc);
    if (!wl_client || !iface)
        return out;
    struct wl_resource* res =
        wl_resource_create(wl_client, iface, version, static_cast<uint32_t>(id));
    if (!res)
        return out;
    out.native_ = res;
    out.id_ = id;
    out.iface_ = iface->name ? iface->name : "";
#else
    (void)client;
    (void)interface_desc;
    (void)version;
    (void)id;
#endif
    return out;
}

unsigned lx::wayland::resource::id() const {
#if defined(LUMEN_HAS_WAYLAND)
    if (native_)
        return wl_resource_get_id(static_cast<struct wl_resource*>(native_));
#endif
    return id_;
}

const char* lx::wayland::resource::interface_name() const { return iface_; }
void* lx::wayland::resource::user_data() const { return user_data_; }
void lx::wayland::resource::set_user_data(void* data) { user_data_ = data; }
void* lx::wayland::resource::native() const { return native_; }

void lx::wayland::resource::set_implementation(const void* impl, void* data,
                                               void (*destroy)(void* wl_resource)) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!native_)
        return;
    user_data_ = data;
    wl_resource_set_implementation(static_cast<struct wl_resource*>(native_), impl, data,
                                   reinterpret_cast<wl_resource_destroy_func_t>(destroy));
#else
    (void)impl;
    (void)data;
    (void)destroy;
    user_data_ = data;
#endif
}

void lx::wayland::resource::post_error(unsigned code, const char* message) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!native_)
        return;
    wl_resource_post_error(static_cast<struct wl_resource*>(native_), code, "%s",
                           message ? message : "");
#else
    (void)code;
    (void)message;
#endif
}

lx::wayland::server::server() = default;

lx::wayland::server::~server() {
    close_listen();
#if defined(LUMEN_HAS_WAYLAND)
    if (display_) {
        // Clients first: wl_display_destroy tears down globals and the event loop but not
        // connected clients, so destroying the display alone leaks every client's
        // server-side state and its resource map. Harmless when the process is exiting
        // anyway, but not when a server is created and destroyed repeatedly — which the
        // dispatch fuzzer does thousands of times a second, and is how this was found.
        wl_display_destroy_clients(display_);
        wl_display_destroy(display_);
        display_ = nullptr;
    }
#endif
    client_count_ = 0;
    global_count_ = 0;
}

void lx::wayland::server::close_listen() {
#if defined(LUMEN_HAS_WAYLAND)
    if (display_) {
        listen_fd_ = -1;
        bound_ = false;
        return;
    }
#endif
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (bound_ && socket_path_[0] != '\0') {
        ::unlink(socket_path_);
        socket_path_[0] = '\0';
    }
    bound_ = false;
}

lx::result<void> lx::wayland::server::bind_raw_socket(const char* socket_name) {
    (void)socket_name;
    return lx::make_error(lx::error_domain::wayland,
                          static_cast<int>(lx::wayland_err::bind_failed),
                          "libwayland-server required for protocol dispatch");
}

#if defined(LUMEN_HAS_WAYLAND)
void lx::wayland::server::advertise_pending_globals() {
    if (!display_)
        return;
    for (unsigned i = 0; i < global_count_; ++i) {
        auto& g = globals_[i];
        if (!g.used || g.wl_global || !g.desc.interface_desc || !g.bind_fn)
            continue;
        auto* iface = static_cast<const struct wl_interface*>(g.desc.interface_desc);
        g.wl_global = wl_global_create(display_, iface, g.desc.version, &g, &global_bind_trampoline);
        if (!g.wl_global) {
            lx::trace::logger::global().log_error(
                lx::make_error(lx::error_domain::wayland,
                               static_cast<int>(lx::wayland_err::bind_failed),
                               "wl_global_create failed"),
                "wayland");
        }
    }
}

lx::result<void> lx::wayland::server::bind_libwayland(const char* socket_name) {
    const char* name = (socket_name && socket_name[0] != '\0') ? socket_name : "lumen-0";

    display_ = wl_display_create();
    if (!display_) {
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "wl_display_create failed");
    }

    if (wl_display_add_socket(display_, name) != 0) {
        wl_display_destroy(display_);
        display_ = nullptr;
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "wl_display_add_socket failed");
    }

    wl_display_init_shm(display_);

    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] != '\0')
        std::snprintf(socket_path_, sizeof(socket_path_), "%s/%s", runtime, name);

    listen_fd_ = wl_event_loop_get_fd(wl_display_get_event_loop(display_));
    bound_ = true;
    advertise_pending_globals();
    return {};
}

void lx::wayland::server::global_bind_trampoline(struct wl_client* client, void* data,
                                                 uint32_t version, uint32_t id) {
    auto* entry = static_cast<global_entry*>(data);
    if (!entry || !entry->bind_fn || !entry->owner || !client)
        return;

    server* srv = entry->owner;
    client_connection* conn = srv->ensure_client(client);
    if (!conn)
        return;

    if (entry->desc.privileged) {
        if (!srv->allow_privileged_global(conn->credentials(), entry->desc.interface_name)) {
            wl_client_post_implementation_error(client, "privileged global denied");
            return;
        }
    }

    const int ver = static_cast<int>(version);
    entry->bind_fn(*conn, static_cast<unsigned>(id), ver);
}
#else
void lx::wayland::server::advertise_pending_globals() {}
#endif

lx::result<void> lx::wayland::server::bind(const char* socket_name) {
    if (bound_) {
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "server already bound");
    }

#if defined(LUMEN_HAS_WAYLAND)
    return bind_libwayland(socket_name);
#else
    return bind_raw_socket(socket_name);
#endif
}

lx::result<void> lx::wayland::server::bind_socketless() {
    if (bound_) {
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "server already bound");
    }

#if defined(LUMEN_HAS_WAYLAND)
    display_ = wl_display_create();
    if (!display_) {
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "wl_display_create failed");
    }
    bound_ = true;
    return {};
#else
    return lx::not_implemented("lx::wayland::server::bind_socketless");
#endif
}

void lx::wayland::server::remove_client_slot(unsigned index) {
    if (index >= client_count_)
        return;
    clients_[index] = {};
    if (index + 1 < client_count_)
        clients_[index] = static_cast<client_connection&&>(clients_[client_count_ - 1]);
    clients_[client_count_ - 1] = {};
    --client_count_;
}

int lx::wayland::server::dispatch(int timeout_ms) {
    if (!bound_)
        return 0;

#if defined(LUMEN_HAS_WAYLAND)
    if (display_) {
        wl_event_loop* loop = wl_display_get_event_loop(display_);
        const int n = wl_event_loop_dispatch(loop, timeout_ms);
        if (n < 0)
            return 0;
        wl_display_flush_clients(display_);
        return n;
    }
#else
    (void)timeout_ms;
#endif
    return 0;
}

void lx::wayland::server::flush() {
#if defined(LUMEN_HAS_WAYLAND)
    if (display_)
        wl_display_flush_clients(display_);
#endif
}

void lx::wayland::server::add_global(global_descriptor desc, global_bind_fn bind_fn) {
    if (global_count_ >= k_max_globals) {
        lx::trace::logger::global().log_error(
            lx::make_error(lx::error_domain::wayland,
                           static_cast<int>(lx::wayland_err::bind_failed),
                           "global table full — interface not advertised"),
            "wayland");
        return;
    }
    if (!bind_fn) {
        lx::trace::logger::global().log(lx::trace::level::warn, "wayland",
                                        "add_global with null bind_fn — not advertised");
        return;
    }
    if (!desc.interface_desc) {
        lx::trace::logger::global().log(lx::trace::level::warn, "wayland",
                                        "add_global with null interface_desc — not advertised");
        return;
    }

    auto& slot = globals_[global_count_];
    slot.desc = desc;
    slot.bind_fn = bind_fn;
    slot.wl_global = nullptr;
    slot.owner = this;
    slot.used = true;
    ++global_count_;

#if defined(LUMEN_HAS_WAYLAND)
    if (display_ && bound_) {
        auto* iface = static_cast<const struct wl_interface*>(desc.interface_desc);
        slot.wl_global =
            wl_global_create(display_, iface, desc.version, &slot, &global_bind_trampoline);
        if (!slot.wl_global) {
            lx::trace::logger::global().log_error(
                lx::make_error(lx::error_domain::wayland,
                               static_cast<int>(lx::wayland_err::bind_failed),
                               "wl_global_create failed"),
                "wayland");
        }
    }
#endif
}

bool lx::wayland::server::allow_privileged_global(const unix_credentials& cred,
                                                  const char* global_name) const {
    (void)global_name;
#if defined(_WIN32)
    (void)cred;
    return false;
#else
    return cred.uid == static_cast<unsigned>(::getuid());
#endif
}

void lx::wayland::server::set_shell_binary_path(const char* path) {
    shell_binary_path_ = path ? path : "lumen-shell";
}

lx::wayland::client_connection* lx::wayland::server::find_client(unsigned id) {
    for (unsigned i = 0; i < client_count_; ++i)
        if (clients_[i].id() == id)
            return &clients_[i];
    return nullptr;
}

lx::wayland::client_connection* lx::wayland::server::find_client_by_native(void* wl_client) {
    if (!wl_client)
        return nullptr;
    for (unsigned i = 0; i < client_count_; ++i)
        if (clients_[i].native() == wl_client)
            return &clients_[i];
    return nullptr;
}

lx::wayland::client_connection* lx::wayland::server::ensure_client(void* wl_client) {
    if (!wl_client)
        return nullptr;
    if (auto* existing = find_client_by_native(wl_client))
        return existing;
    if (client_count_ >= k_max_clients)
        return nullptr;

    unix_credentials cred{};
#if defined(LUMEN_HAS_WAYLAND)
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(static_cast<struct wl_client*>(wl_client), &pid, &uid, &gid);
    cred.pid = static_cast<unsigned>(pid);
    cred.uid = static_cast<unsigned>(uid);
    cred.gid = static_cast<unsigned>(gid);
#endif

    auto& slot = clients_[client_count_++];
    slot.set_id(next_client_id_++);
    slot.set_credentials(cred);
    slot.set_native(wl_client);
    slot.set_server(this);
#if !defined(_WIN32)
    slot.set_privileged_shell(cred.uid == static_cast<unsigned>(::getuid()));
#endif

#if defined(LUMEN_HAS_WAYLAND)
    const unsigned idx = client_count_ - 1;
    auto& link = destroy_links_[idx];
    link.server = this;
    link.client_id = slot.id();
    wl_list_init(&link.listener.link);
    link.listener.notify = [](struct wl_listener* listener, void* /*data*/) {
        auto* self = reinterpret_cast<client_destroy_link*>(
            reinterpret_cast<char*>(listener) - offsetof(client_destroy_link, listener));
        if (!self->server)
            return;
        for (unsigned i = 0; i < self->server->client_count_; ++i) {
            if (self->server->clients_[i].id() == self->client_id) {
                self->server->remove_client_slot(i);
                break;
            }
        }
    };
    wl_client_add_destroy_listener(static_cast<struct wl_client*>(wl_client), &link.listener);
#endif
    return &slot;
}

void* lx::wayland::server::native_display() const {
#if defined(LUMEN_HAS_WAYLAND)
    return display_;
#else
    return nullptr;
#endif
}

int lx::wayland::server::event_fd() const { return listen_fd_; }
