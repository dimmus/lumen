module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <limits.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

import lx.foundation;
import lx.runtime;
import lx.session;
import lx.trace;
import lx.wayland.protocols;

export module lx.wayland.server;

import std;

namespace {

constexpr lx::trace::category log_wayland{"wayland"};

/// Whether the kernel gives out peer pidfds (SO_PEERPIDFD, Linux 6.5): probed once per
/// process, at the first bind, and logged then.
bool peer_pidfds() {
    static const bool supported = [] {
        const bool ok = lx::session::peer_pidfd_supported();
        if (ok)
            log_wayland.info("client pidfds: SO_PEERPIDFD, /proc/<pid> checks race-free");
        else
            log_wayland.info("client pidfds unavailable (SO_PEERPIDFD, Linux 6.5): "
                             "/proc/<pid> checks use the SO_PEERCRED pid alone");
        return ok;
    }();
    return supported;
}

} // namespace

export namespace lx::wayland {

struct unix_credentials {
    unsigned pid = 0;
    unsigned uid = 0;
    unsigned gid = 0;
    /// The peer's pidfd (SO_PEERPIDFD at connect), owned by its `client_connection`; -1
    /// without one. An answer read through `/proc/<pid>` counts only if this still names a
    /// running process afterwards — otherwise the pid may be another process's by now.
    int pidfd = -1;
    /// The kernel has SO_PEERPIDFD but gave no pidfd: the peer had already exited, so no
    /// `/proc/<pid>` answer is its. With neither (kernels before 6.5) the pid is taken alone.
    bool peer_gone = false;
};

class client_connection;
class resource;
class server;

/// wp_security_context_v1 metadata a sandbox engine attached to a listening socket
/// (security-context-v1); every client accepted on that socket carries it. Each string is
/// empty when not set.
struct security_context {
    static constexpr unsigned k_bytes = 256;
    char sandbox_engine[k_bytes]{};
    char app_id[k_bytes]{};
    char instance_id[k_bytes]{};
};

/// Opaque bind callback: client, object id, interface version.
using global_bind_fn = void (*)(client_connection& client, unsigned id, int version);
/// The same with the global's own user pointer (`server::add_global_ex`); null once the
/// global was removed (`server::remove_global`) — a bind racing the removal gets an inert
/// object.
using global_bind_ex_fn = void (*)(client_connection& client, unsigned id, int version,
                                   void* user);

/// A global added with `server::add_global_ex`: its table slot, and the slot's generation
/// (a slot is reused once its old global is destroyed, so a stale handle never matches).
struct global_handle {
    unsigned index = ~0u;
    unsigned generation = 0;
    [[nodiscard]] bool valid() const { return index != ~0u; }
};

class client_connection {
public:
    [[nodiscard]] unsigned id() const;
    [[nodiscard]] unix_credentials credentials() const;
    [[nodiscard]] bool is_privileged_shell() const;
    /// The client runs in a sandbox: it connected through a security-context listener
    /// (security-context-v1, what Flatpak 1.16 and later use), or — for older Flatpak —
    /// `/proc/<pid>/root/.flatpak-info` exists. The file check runs once, at the first
    /// global filtered for it, and is cached for its life.
    [[nodiscard]] bool sandboxed();
    /// The security context it connected with, or null (it connected on the display's own
    /// socket).
    [[nodiscard]] const security_context* context() const {
        return has_context_ ? &context_ : nullptr;
    }

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
    lx::unique_fd pidfd_; ///< closed when the client leaves the table
    void* native_ = nullptr; // wl_client*
    server* server_ = nullptr;
    bool privileged_shell_ = false;
    /// 0 not checked yet, 1 not sandboxed, 2 sandboxed.
    unsigned char sandbox_ = 0;
    bool has_context_ = false;
    security_context context_{};
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
    /// Hidden from sandboxed clients (`server`'s global filter, ADR 008 C5): globals that
    /// configure or watch the whole session — output management and power, idle
    /// notification, input methods, virtual keyboards, the session lock, layer shell — are
    /// for same-UID tools (wlr-randr, kanshi, swayidle, fcitx5, a panel), not for a Flatpak
    /// app. Hidden means absent from its registry, and a bind by name is refused by
    /// libwayland as an invalid global.
    bool deny_sandboxed = false;
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
    /// Dispatches client requests. `flush` sends what they produced at once; a caller that
    /// flushes anyway once its whole iteration is done (the compositor's event loop) passes
    /// false, so a client gets one send per iteration instead of one per dispatch.
    [[nodiscard]] int dispatch(int timeout_ms, bool flush = true);
    void flush();
    /// Disconnects every client now (their objects' destructors run here), so an owner can
    /// tear down while what those destructors touch is still alive. Idempotent; the
    /// destructor does it too.
    void destroy_clients();

    void add_global(global_descriptor desc, global_bind_fn bind_fn);
    /// A global with a user pointer of its own, which can be removed again — one
    /// `wl_output` per output (ADR 008 H4). Invalid handle when the table is full.
    [[nodiscard]] global_handle add_global_ex(global_descriptor desc, global_bind_ex_fn bind_fn,
                                              void* user);
    /// Withdraws a global the safe way (wlroots' `wlr_global_destroy_safe`):
    /// `wl_global_remove` now, so clients get `global_remove`, and `wl_global_destroy`
    /// `k_global_destroy_delay_ns` later (`collect_globals`), so a client that binds before
    /// it has processed the removal does not bind a destroyed global — a fatal error for it.
    /// Binds in between reach the bind function with a null user pointer.
    void remove_global(global_handle handle, unsigned long long now_ns);
    /// Destroys removed globals whose delay is over and frees their table slots (the
    /// housekeeping tick). Cheap when there is none.
    void collect_globals(unsigned long long now_ns);
    /// Removed globals still waiting to be destroyed.
    [[nodiscard]] unsigned removed_global_count() const { return removed_count_; }
    static constexpr unsigned long long k_global_destroy_delay_ns = 5'000'000'000ull;
    /// The privilege check for `zlm_shell_v1` (architecture §9): the peer runs as this
    /// session's UID **and** its executable (`/proc/<pid>/exe`, as the kernel names it) is
    /// the configured shell binary. A denial is logged at warn with both paths.
    [[nodiscard]] bool allow_privileged_global(const unix_credentials& cred,
                                               const char* global_name) const;

    /// The shell binary privileged binds must come from. A bare name ("lumen-shell", the
    /// default) is looked up next to the running compositor's own executable; any other
    /// path is taken relative to the working directory. Either way it is resolved once
    /// here, to the canonical path the kernel reports for a running executable.
    void set_shell_binary_path(const char* path);
    [[nodiscard]] const char* shell_binary_path() const { return shell_binary_path_; }

    [[nodiscard]] client_connection* find_client(unsigned id);
    [[nodiscard]] client_connection* find_client_by_native(void* wl_client);
    [[nodiscard]] client_connection* ensure_client(void* wl_client);

    /// Called once per client from its destroy signal, after it has left this table and
    /// before libwayland destroys its objects: state keyed by the `wl_client*` (which may
    /// be reused afterwards) is dropped here. One hook; installed by the protocol layer.
    using client_destroyed_fn = void (*)(void* user, unsigned client_id, void* wl_client);
    void set_client_destroyed_hook(client_destroyed_fn fn, void* user) {
        client_destroyed_ = fn;
        client_destroyed_user_ = user;
    }

    /// security-context-v1: accepts clients on `listen_fd` (a listening AF_UNIX stream
    /// socket) with `context` attached — they are sandboxed — until `close_fd` hangs up.
    /// Both fds are owned from here on (closed on failure too). Listening goes on after the
    /// creating client (`creator`, its id) disconnects, as the protocol requires. At most
    /// `k_max_security_listeners`, `k_max_security_listeners_per_client` per creator.
    [[nodiscard]] lx::result<void> add_security_listener(int listen_fd, int close_fd,
                                                         const security_context& context,
                                                         unsigned creator);
    [[nodiscard]] unsigned security_listener_count() const;
    static constexpr unsigned k_max_security_listeners = 32;
    static constexpr unsigned k_max_security_listeners_per_client = 8;

    /// Native `wl_display*` (nullptr when unavailable).
    [[nodiscard]] void* native_display() const;
    /// At least one client is connected.
    [[nodiscard]] bool has_clients() const;
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
        global_bind_ex_fn bind_ex_fn = nullptr;
        void* user = nullptr;
        void* wl_global = nullptr; // wl_global*
        server* owner = nullptr;
        /// Bumped whenever the slot is (re)used.
        unsigned generation = 0;
        /// Removed (`global_remove` sent); destroyed at `destroy_at_ns`.
        bool removed = false;
        unsigned long long destroy_at_ns = 0;
        bool used = false;
    };
    /// A free table slot (one freed by `collect_globals` first), or `k_max_globals`.
    [[nodiscard]] unsigned alloc_global_slot();
    void create_wl_global(global_entry& slot);

    [[nodiscard]] lx::result<void> bind_raw_socket(const char* socket_name);
#if defined(LUMEN_HAS_WAYLAND)
    [[nodiscard]] lx::result<void> bind_libwayland(const char* socket_name);
    static void global_bind_trampoline(struct wl_client* client, void* data, uint32_t version,
                                       uint32_t id);
    /// `wl_display_set_global_filter`: hides `deny_sandboxed` and `privileged` globals from
    /// sandboxed clients.
    static bool global_filter(const struct wl_client* client, const struct wl_global* global,
                              void* data);
#endif
    void close_listen();
    void remove_client_slot(unsigned index);
    void advertise_pending_globals();

    client_connection clients_[k_max_clients]{};
    unsigned client_count_ = 0;
    unsigned next_client_id_ = 1;
    global_entry globals_[k_max_globals]{};
    unsigned global_count_ = 0;
    unsigned removed_count_ = 0;

    int listen_fd_ = -1;
    char socket_path_[108]{};
    bool bound_ = false;
    client_destroyed_fn client_destroyed_ = nullptr;
    void* client_destroyed_user_ = nullptr;
    char shell_binary_path_[PATH_MAX]{};

#if defined(LUMEN_HAS_WAYLAND)
    wl_display* display_ = nullptr;

    struct client_destroy_link {
        wl_listener listener{};
        server* server = nullptr;
        unsigned client_id = 0;
    };
    client_destroy_link destroy_links_[k_max_clients]{};

    /// security-context listeners: sources on libwayland's own event loop (the display's
    /// fd is what the runtime loop watches), so they cost no runtime-loop slot.
    struct security_listener {
        server* owner = nullptr;
        int listen_fd = -1;
        int close_fd = -1;
        wl_event_source* accept_source = nullptr;
        wl_event_source* close_source = nullptr;
        unsigned creator = 0;
        security_context context{};
        bool used = false;
    };
    security_listener listeners_[k_max_security_listeners]{};
    static int on_listener_readable(int fd, uint32_t mask, void* data);
    static int on_listener_closed(int fd, uint32_t mask, void* data);
    void drop_listener(security_listener& l);
    void accept_sandboxed(security_listener& l);
#endif
};

} // namespace lx::wayland

module :private;

unsigned lx::wayland::client_connection::id() const { return id_; }
lx::wayland::unix_credentials lx::wayland::client_connection::credentials() const {
    unix_credentials out = cred_;
    out.pidfd = pidfd_.get();
    return out;
}
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

namespace {
/// An answer just read through `/proc/<cred.pid>` is about the peer: its pidfd still names a
/// process that has not exited (`pidfd_alive`: not a zombie either), so the pid was never
/// free to be reused in between.
bool peer_confirmed(const lx::wayland::unix_credentials& cred) {
    if (cred.pidfd >= 0)
        return lx::session::pidfd_alive(cred.pidfd);
    return !cred.peer_gone;
}
} // namespace

bool lx::wayland::client_connection::sandboxed() {
    if (has_context_)
        return true;
    if (sandbox_ != 0)
        return sandbox_ == 2;
    sandbox_ = 1;
    // Flatpak puts /.flatpak-info at the root of every sandbox (xdg-desktop-portal reads
    // the same file to identify apps); a same-UID process may look into another's root. The
    // peer's pid (SO_PEERCRED at connect) can be reused once the peer exits — its connection
    // may live on in a child — so with a pidfd the root is opened first and then confirmed
    // through it, and the file looked up in that root. A root that cannot be opened (the
    // peer is a zombie, its leader exited, it is not dumpable) or a peer that is gone counts
    // as sandboxed. Without a pidfd (kernels before 6.5) a failed lookup counts as not
    // sandboxed: the global stays as it was before filters existed.
    if (cred_.pid == 0)
        return false;
    const auto cred = credentials();
    char path[64];
    bool found = false;
    if (cred.pidfd >= 0) {
        std::snprintf(path, sizeof(path), "/proc/%u/root", cred_.pid);
        const lx::unique_fd root{::open(path, O_PATH | O_DIRECTORY | O_CLOEXEC)};
        if (root.get() < 0 || !peer_confirmed(cred)) {
            sandbox_ = 2;
            log_wayland.info("client pid={} has no readable root or has exited before its "
                             "sandbox check: treated as sandboxed",
                             cred_.pid);
            return true;
        }
        found = ::faccessat(root.get(), ".flatpak-info", F_OK, 0) == 0;
    } else if (cred.peer_gone) {
        sandbox_ = 2;
        log_wayland.info("client pid={} exited before its sandbox check: treated as "
                         "sandboxed",
                         cred_.pid);
        return true;
    } else {
        std::snprintf(path, sizeof(path), "/proc/%u/root/.flatpak-info", cred_.pid);
        found = ::access(path, F_OK) == 0;
    }
    if (found) {
        sandbox_ = 2;
        log_wayland.info("client pid={} is sandboxed (Flatpak): session-wide "
                         "globals hidden",
                         cred_.pid);
    }
    return sandbox_ == 2;
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
    for (auto& l : listeners_)
        if (l.used)
            drop_listener(l);
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
    return lx::make_error(lx::wayland_err::bind_failed,
                          "libwayland-server required for protocol dispatch");
}

#if defined(LUMEN_HAS_WAYLAND)
void lx::wayland::server::advertise_pending_globals() {
    if (!display_)
        return;
    for (unsigned i = 0; i < global_count_; ++i) {
        auto& g = globals_[i];
        if (!g.used || g.removed || g.wl_global || !g.desc.interface_desc ||
            (!g.bind_fn && !g.bind_ex_fn))
            continue;
        auto* iface = static_cast<const struct wl_interface*>(g.desc.interface_desc);
        g.wl_global = wl_global_create(display_, iface, g.desc.version, &g, &global_bind_trampoline);
        if (!g.wl_global) {
            log_wayland.error(
                lx::make_error(lx::wayland_err::bind_failed, "wl_global_create failed"));
        }
    }
}

#if defined(LUMEN_HAS_WAYLAND)
namespace {
/// Per-client connection buffer: 1 MiB instead of libwayland's 4 KiB default (KWin MR 5984,
/// "1 MiB ≈ 30 s of unprocessed motion"). With 4 KiB libwayland disconnects a client that
/// stops reading for a hundred pointer motions — a stopped or hung client would be killed by
/// the protocol library, and Lumen never kills clients for being slow. The memory is only
/// used while a client is behind.
void raise_client_buffers(wl_display* display) {
#if WAYLAND_VERSION_MAJOR > 1 || (WAYLAND_VERSION_MAJOR == 1 && WAYLAND_VERSION_MINOR >= 23)
    wl_display_set_default_max_buffer_size(display, 1u << 20);
#else
    (void)display;
#endif
}
} // namespace
#endif

lx::result<void> lx::wayland::server::bind_libwayland(const char* socket_name) {
    const char* name = (socket_name && socket_name[0] != '\0') ? socket_name : "lumen-0";

    display_ = wl_display_create();
    if (!display_) {
        return lx::make_error(lx::wayland_err::bind_failed, "wl_display_create failed");
    }
    raise_client_buffers(display_);

    if (wl_display_add_socket(display_, name) != 0) {
        wl_display_destroy(display_);
        display_ = nullptr;
        return lx::make_error(lx::wayland_err::bind_failed, "wl_display_add_socket failed");
    }

    wl_display_init_shm(display_);
    wl_display_set_global_filter(display_, &global_filter, this);
    (void)peer_pidfds();

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
    if (!entry || (!entry->bind_fn && !entry->bind_ex_fn) || !entry->owner || !client)
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
    if (entry->bind_ex_fn) {
        // A removed global's bind: the user pointer is gone, the object will be inert.
        entry->bind_ex_fn(*conn, static_cast<unsigned>(id), ver,
                          entry->removed ? nullptr : entry->user);
        return;
    }
    entry->bind_fn(*conn, static_cast<unsigned>(id), ver);
}

bool lx::wayland::server::global_filter(const struct wl_client* client,
                                        const struct wl_global* global, void* data) {
    auto* srv = static_cast<server*>(data);
    if (!srv || !client || !global)
        return true;
    // Only globals of this table carry a descriptor; libwayland's own (wl_shm) are visible.
    const auto at = reinterpret_cast<std::uintptr_t>(wl_global_get_user_data(global));
    const auto lo = reinterpret_cast<std::uintptr_t>(srv->globals_);
    const auto hi = reinterpret_cast<std::uintptr_t>(srv->globals_ + k_max_globals);
    if (at < lo || at >= hi)
        return true;
    const auto* entry = reinterpret_cast<const global_entry*>(at);
    // Privileged globals (zlm_shell_v1) too: a sandboxed client is never the shell, so its
    // bind would only be refused — it does not see them at all.
    if (!entry->desc.deny_sandboxed && !entry->desc.privileged)
        return true;
    client_connection* conn = srv->ensure_client(const_cast<struct wl_client*>(client));
    return !conn || !conn->sandboxed();
}
#else
void lx::wayland::server::advertise_pending_globals() {}
#endif

lx::result<void> lx::wayland::server::bind(const char* socket_name) {
    if (bound_) {
        return lx::make_error(lx::wayland_err::bind_failed, "server already bound");
    }

#if defined(LUMEN_HAS_WAYLAND)
    return bind_libwayland(socket_name);
#else
    return bind_raw_socket(socket_name);
#endif
}

lx::result<void> lx::wayland::server::bind_socketless() {
    if (bound_) {
        return lx::make_error(lx::wayland_err::bind_failed, "server already bound");
    }

#if defined(LUMEN_HAS_WAYLAND)
    display_ = wl_display_create();
    if (!display_) {
        return lx::make_error(lx::wayland_err::bind_failed, "wl_display_create failed");
    }
    raise_client_buffers(display_);
    wl_display_set_global_filter(display_, &global_filter, this);
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
        clients_[index] = std::move(clients_[client_count_ - 1]);
    clients_[client_count_ - 1] = {};
    --client_count_;
}

int lx::wayland::server::dispatch(int timeout_ms, bool flush) {
    if (!bound_)
        return 0;

#if defined(LUMEN_HAS_WAYLAND)
    if (display_) {
        wl_event_loop* loop = wl_display_get_event_loop(display_);
        const int n = wl_event_loop_dispatch(loop, timeout_ms);
        if (n < 0)
            return 0;
        if (flush)
            wl_display_flush_clients(display_);
        return n;
    }
#else
    (void)timeout_ms;
    (void)flush;
#endif
    return 0;
}

void lx::wayland::server::destroy_clients() {
#if defined(LUMEN_HAS_WAYLAND)
    if (display_)
        wl_display_destroy_clients(display_);
#endif
}

void lx::wayland::server::flush() {
#if defined(LUMEN_HAS_WAYLAND)
    if (display_)
        wl_display_flush_clients(display_);
#endif
}

unsigned lx::wayland::server::alloc_global_slot() {
    for (unsigned i = 0; i < global_count_; ++i)
        if (!globals_[i].used)
            return i;
    return global_count_ < k_max_globals ? global_count_++ : k_max_globals;
}

void lx::wayland::server::create_wl_global(global_entry& slot) {
#if defined(LUMEN_HAS_WAYLAND)
    if (display_ && bound_) {
        auto* iface = static_cast<const struct wl_interface*>(slot.desc.interface_desc);
        slot.wl_global =
            wl_global_create(display_, iface, slot.desc.version, &slot, &global_bind_trampoline);
        if (!slot.wl_global) {
            log_wayland.error(
                lx::make_error(lx::wayland_err::bind_failed, "wl_global_create failed"));
        }
    }
#else
    (void)slot;
#endif
}

lx::wayland::global_handle lx::wayland::server::add_global_ex(global_descriptor desc,
                                                              global_bind_ex_fn bind_fn,
                                                              void* user) {
    if (!bind_fn || !desc.interface_desc) {
        log_wayland.warn("add_global_ex without a bind function or interface");
        return {};
    }
    const unsigned index = alloc_global_slot();
    if (index >= k_max_globals) {
        log_wayland.error(lx::make_error(lx::wayland_err::bind_failed,
                                         "global table full — interface not advertised"));
        return {};
    }
    auto& slot = globals_[index];
    const unsigned generation = slot.generation + 1;
    slot = {};
    slot.desc = desc;
    slot.bind_ex_fn = bind_fn;
    slot.user = user;
    slot.owner = this;
    slot.used = true;
    slot.generation = generation;
    create_wl_global(slot);
    return {index, generation};
}

void lx::wayland::server::remove_global(global_handle handle, unsigned long long now_ns) {
    if (!handle.valid() || handle.index >= global_count_)
        return;
    auto& slot = globals_[handle.index];
    if (!slot.used || slot.removed || slot.generation != handle.generation)
        return;
    slot.removed = true;
    slot.user = nullptr;
    slot.destroy_at_ns = now_ns + k_global_destroy_delay_ns;
    ++removed_count_;
#if defined(LUMEN_HAS_WAYLAND)
    if (slot.wl_global)
        wl_global_remove(static_cast<wl_global*>(slot.wl_global));
#endif
}

void lx::wayland::server::collect_globals(unsigned long long now_ns) {
    if (removed_count_ == 0)
        return;
    for (unsigned i = 0; i < global_count_; ++i) {
        auto& slot = globals_[i];
        if (!slot.used || !slot.removed || now_ns < slot.destroy_at_ns)
            continue;
#if defined(LUMEN_HAS_WAYLAND)
        if (slot.wl_global)
            wl_global_destroy(static_cast<wl_global*>(slot.wl_global));
#endif
        const unsigned generation = slot.generation;
        slot = {};
        slot.generation = generation;
        --removed_count_;
    }
}

void lx::wayland::server::add_global(global_descriptor desc, global_bind_fn bind_fn) {
    if (!bind_fn) {
        log_wayland.warn("add_global with null bind_fn — not advertised");
        return;
    }
    if (!desc.interface_desc) {
        log_wayland.warn("add_global with null interface_desc — not advertised");
        return;
    }
    const unsigned index = alloc_global_slot();
    if (index >= k_max_globals) {
        log_wayland.error(lx::make_error(lx::wayland_err::bind_failed,
                                         "global table full — interface not advertised"));
        return;
    }
    auto& slot = globals_[index];
    const unsigned generation = slot.generation + 1;
    slot = {};
    slot.desc = desc;
    slot.bind_fn = bind_fn;
    slot.owner = this;
    slot.used = true;
    slot.generation = generation;
    create_wl_global(slot);
}

bool lx::wayland::server::allow_privileged_global(const unix_credentials& cred,
                                                  const char* global_name) const {
    const char* name = global_name ? global_name : "?";
    if (cred.uid != static_cast<unsigned>(::getuid())) {
        log_wayland.warn("privileged global {} denied: client uid={} pid={}", name, cred.uid,
                         cred.pid);
        return false;
    }
    // The peer's pid comes from SO_PEERCRED at connect. An open connection does not keep
    // that process alive (a child may hold it), so after the read its pidfd must still name
    // a running process — then the pid was never free to be reused, and the executable is
    // the peer's. An executable that cannot be read (a zombie, a group whose leader exited)
    // is a refusal too. Without SO_PEERPIDFD the pid is taken alone. Same check as KWin's
    // restricted interfaces (it reads /proc/<pid>/exe too). It is not a boundary against a
    // same-UID attacker that can ptrace the shell — nothing at this layer is.
    char exe[PATH_MAX]{};
    bool have_exe = false;
    if (cred.pid != 0) {
        char proc[32];
        std::snprintf(proc, sizeof(proc), "/proc/%u/exe", cred.pid);
        const ssize_t n = ::readlink(proc, exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            have_exe = true;
        }
    }
    if (!peer_confirmed(cred)) {
        log_wayland.warn("privileged global {} denied: client pid={} has exited, its pid may "
                         "name another process",
                         name, cred.pid);
        return false;
    }
    const lx::session::privilege_checker checker{
        {.expected_uid = static_cast<unsigned>(::getuid()),
         .shell_binary_path = shell_binary_path_,
         .require_flatpak_token = false}};
    const auto verdict = checker.check_shell_bind(
        lx::session::credentials{.uid = cred.uid, .gid = cred.gid, .pid = cred.pid},
        have_exe ? exe : nullptr, nullptr);
    if (verdict == lx::session::privilege_result::allowed)
        return true;
    log_wayland.warn("privileged global {} denied: client pid={} exe={} is not "
                     "the shell binary {}",
                     name, cred.pid, have_exe ? exe : "(unknown)", shell_binary_path_);
    return false;
}

void lx::wayland::server::set_shell_binary_path(const char* path) {
    if (!path || !path[0])
        path = "lumen-shell";
    char candidate[PATH_MAX]{};
    if (std::strchr(path, '/') == nullptr) {
        // Next to this executable: an installed or freshly built pair always matches.
        char self[PATH_MAX]{};
        const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
        char* slash = n > 0 ? std::strrchr(self, '/') : nullptr;
        if (slash) {
            *slash = '\0';
            std::snprintf(candidate, sizeof(candidate), "%s/%s", self, path);
        } else {
            std::snprintf(candidate, sizeof(candidate), "%s", path);
        }
    } else {
        std::snprintf(candidate, sizeof(candidate), "%s", path);
    }
    if (::realpath(candidate, shell_binary_path_) == nullptr) {
        // Kept as given: nothing can match it, so every privileged bind is denied (and
        // each denial names this path), rather than the check silently passing.
        std::snprintf(shell_binary_path_, sizeof(shell_binary_path_), "%s", candidate);
        log_wayland.warn("shell binary {} not found: zlm_shell_v1 binds will be "
                         "denied",
                         shell_binary_path_);
        return;
    }
    log_wayland.info("shell binary {}", shell_binary_path_);
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
    // The pidfd names the process the kernel recorded at connect, however late it is asked.
    lx::unique_fd pidfd;
    if (peer_pidfds()) {
        auto fd =
            lx::session::peer_pidfd(wl_client_get_fd(static_cast<struct wl_client*>(wl_client)));
        if (fd)
            pidfd = std::move(*fd);
        else
            cred.peer_gone = true;
    }
#endif

    auto& slot = clients_[client_count_++];
    slot.set_id(next_client_id_++);
    slot.set_credentials(cred);
#if defined(LUMEN_HAS_WAYLAND)
    slot.pidfd_ = std::move(pidfd);
#endif
    slot.set_native(wl_client);
    slot.set_server(this);
    slot.set_privileged_shell(cred.uid == static_cast<unsigned>(::getuid()));

#if defined(LUMEN_HAS_WAYLAND)
    // A link stays where it is for its client's whole life: the listener is an element of
    // that client's destroy-signal list. Links are not tied to slot indices, because
    // remove_client_slot moves the last slot into the hole — a link indexed by slot would be
    // re-initialised for the next client while still linked into another client's list, and
    // that client's destroy then loops forever. A link is free again once it has fired
    // (libwayland unlinks each listener of a client's final destroy emission).
    client_destroy_link* link = nullptr;
    for (auto& l : destroy_links_) {
        if (!l.server) {
            link = &l;
            break;
        }
    }
    if (!link) {
        --client_count_;
        slot = {};
        return nullptr;
    }
    link->server = this;
    link->client_id = slot.id();
    wl_list_init(&link->listener.link);
    link->listener.notify = [](struct wl_listener* listener, void* data) {
        auto* self = reinterpret_cast<client_destroy_link*>(
            reinterpret_cast<char*>(listener) - offsetof(client_destroy_link, listener));
        server* srv = self->server;
        if (!srv)
            return;
        self->server = nullptr;
        for (unsigned i = 0; i < srv->client_count_; ++i) {
            if (srv->clients_[i].id() == self->client_id) {
                srv->remove_client_slot(i);
                break;
            }
        }
        if (srv->client_destroyed_)
            srv->client_destroyed_(srv->client_destroyed_user_, self->client_id, data);
    };
    wl_client_add_destroy_listener(static_cast<struct wl_client*>(wl_client), &link->listener);
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

bool lx::wayland::server::has_clients() const {
#if defined(LUMEN_HAS_WAYLAND)
    return display_ && !wl_list_empty(wl_display_get_client_list(display_));
#else
    return client_count_ > 0;
#endif
}

int lx::wayland::server::event_fd() const { return listen_fd_; }

#if defined(LUMEN_HAS_WAYLAND)
void lx::wayland::server::drop_listener(security_listener& l) {
    if (l.accept_source)
        wl_event_source_remove(l.accept_source);
    if (l.close_source)
        wl_event_source_remove(l.close_source);
    if (l.listen_fd >= 0)
        ::close(l.listen_fd);
    if (l.close_fd >= 0)
        ::close(l.close_fd);
    l = {};
}

int lx::wayland::server::on_listener_closed(int, uint32_t, void* data) {
    auto* l = static_cast<security_listener*>(data);
    server* srv = l ? l->owner : nullptr;
    if (!srv)
        return 0;
    log_wayland.info("security context {}/{}: listener closed by the sandbox "
                     "engine",
                     l->context.sandbox_engine[0] ? l->context.sandbox_engine : "-",
                     l->context.app_id[0] ? l->context.app_id : "-");
    srv->drop_listener(*l);
    return 0;
}

int lx::wayland::server::on_listener_readable(int, uint32_t mask, void* data) {
    auto* l = static_cast<security_listener*>(data);
    server* srv = l ? l->owner : nullptr;
    if (!srv)
        return 0;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        srv->drop_listener(*l);
        return 0;
    }
    srv->accept_sandboxed(*l);
    return 0;
}

void lx::wayland::server::accept_sandboxed(security_listener& l) {
    const int fd = ::accept4(l.listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0)
        return; // EAGAIN (another accept raced it) or a transient error: the source stays
    wl_client* client = wl_client_create(display_, fd);
    if (!client) {
        ::close(fd);
        return;
    }
    // Registered before it can send a request: the context must be in place before the
    // first global is filtered for it. Without a slot it cannot be marked sandboxed, so it
    // is not let in at all.
    client_connection* conn = ensure_client(client);
    if (!conn) {
        wl_client_destroy(client);
        log_wayland.warn("sandboxed client refused: client table full");
        return;
    }
    conn->context_ = l.context;
    conn->has_context_ = true;
    conn->sandbox_ = 2;
    log_wayland.info("client {} connected sandboxed (security context engine={} "
                     "app_id={}): session-wide globals hidden",
                     conn->id(), l.context.sandbox_engine[0] ? l.context.sandbox_engine : "-",
                     l.context.app_id[0] ? l.context.app_id : "-");
}
#endif

lx::result<void> lx::wayland::server::add_security_listener(int listen_fd, int close_fd,
                                                            const security_context& context,
                                                            unsigned creator) {
#if defined(LUMEN_HAS_WAYLAND)
    security_listener* slot = nullptr;
    unsigned of_creator = 0;
    for (auto& l : listeners_) {
        if (!l.used) {
            if (!slot)
                slot = &l;
        } else if (l.creator == creator) {
            ++of_creator;
        }
    }
    if (!display_ || !slot || of_creator >= k_max_security_listeners_per_client) {
        ::close(listen_fd);
        ::close(close_fd);
        return lx::make_error(lx::wayland_err::bind_failed, "security context listener table full");
    }
    // accept4 after readability must not block if another accept took the connection.
    const int flags = ::fcntl(listen_fd, F_GETFL);
    if (flags >= 0)
        (void)::fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    *slot = {};
    slot->owner = this;
    slot->listen_fd = listen_fd;
    slot->close_fd = close_fd;
    slot->creator = creator;
    slot->context = context;
    slot->used = true;
    wl_event_loop* loop = wl_display_get_event_loop(display_);
    slot->accept_source =
        wl_event_loop_add_fd(loop, listen_fd, WL_EVENT_READABLE, &on_listener_readable, slot);
    // Mask 0: only hangup and error, which epoll always reports (wlroots does the same).
    slot->close_source = wl_event_loop_add_fd(loop, close_fd, 0, &on_listener_closed, slot);
    if (!slot->accept_source || !slot->close_source) {
        drop_listener(*slot);
        return lx::make_error(lx::wayland_err::bind_failed,
                              "security context listener: event source failed");
    }
    return {};
#else
    (void)context;
    (void)creator;
    ::close(listen_fd);
    ::close(close_fd);
    return lx::not_implemented("lx::wayland::server::add_security_listener");
#endif
}

unsigned lx::wayland::server::security_listener_count() const {
#if defined(LUMEN_HAS_WAYLAND)
    unsigned n = 0;
    for (const auto& l : listeners_)
        n += l.used ? 1u : 0u;
    return n;
#else
    return 0;
#endif
}
