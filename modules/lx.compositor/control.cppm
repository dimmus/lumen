module;

// The compositor's control socket (design-session-and-wm S.7, S.8): a Varlink service,
// interface io.lumen.Compositor, on $XDG_RUNTIME_DIR/<display>.varlink, served with
// sd-varlink on a private sd-event loop whose one fd the UI loop waits on. lumen-ctl is its
// client; `varlinkctl introspect` describes it. Only this UID may connect
// (SD_VARLINK_SERVER_MYSELF_ONLY, plus mode 0600).
//
// The partition knows nothing of the compositor: what a method returns comes through a
// `control_host` table the compositor fills, in plain structs. Every handler runs on the UI
// thread, inside `dispatch()`. Declarations only; the bodies (and every sd-* type) are in
// control.cpp.

import lx.foundation;
import lx.health;

export module lx.compositor:control;

import std;

export namespace lx::compositor {

/// The interface name. Provisional until the project owns a domain.
inline constexpr const char* k_control_interface = "io.lumen.Compositor";

struct control_version {
    const char* version = "";
    unsigned zlm_version = 0;
    const char* backend = "";
};

struct control_output {
    char name[32]{};
    char description[160]{};
    char identifier[208]{};
    bool enabled = true;
    /// Logical (layout) rect.
    int x = 0, y = 0, width = 0, height = 0;
    int mode_width = 0, mode_height = 0;
    unsigned refresh_mhz = 0;
    double scale = 1.0;
    /// What clients are hinted to render at (the scale when no render scale is set).
    double render_scale = 1.0;
    const char* transform = "normal";
    /// The name of its active workspace ("" none).
    char active_workspace[32]{};
};

/// One workspace (io.lumen.Compositor `Workspace`).
struct control_workspace {
    unsigned id = 0;
    /// The output's name ("" while stranded: no output).
    char output[32]{};
    /// 1-based position on its output (what workspace.switch N takes).
    unsigned index = 0;
    char name[32]{};
    bool active = false;
    bool urgent = false;
    unsigned windows = 0;
};

struct control_window {
    unsigned id = 0;
    char app_id[128]{};
    char title[256]{};
    /// 0: unknown.
    int pid = 0;
    /// Empty: on no output.
    char output[32]{};
    /// The workspace it belongs to (0: none — sticky, or no workspace yet).
    unsigned workspace = 0;
    /// "normal", "maximized", "fullscreen", "minimized", "tiled", "modal".
    const char* state = "normal";
    bool focused = false;
    bool minimized = false;
    bool sticky = false;
    int x = 0, y = 0, width = 0, height = 0;
};

/// One problem a configuration reload found (io.lumen.Compositor `Diagnostic`).
struct control_diagnostic {
    const char* severity = "error";
    const char* file = "";
    unsigned line = 0;
    const char* key = "";
    const char* message = "";
};

/// What `RunAction` did.
enum class control_action_result : unsigned char {
    ok,
    unknown_action,
    invalid_argument,
    shell_not_connected,
    session_locked,
};

/// The compositor's side. Every function runs on the UI thread from inside `dispatch()`.
struct control_host {
    void (*version)(void* user, control_version& out) = nullptr;
    /// A GetHealth arrived. The host answers with `control_server::finish_health(token, …)`
    /// — at once or later (the compositor asks the render thread first). If it has not
    /// within `health_timeout_ms`, `health_timeout(token)` is called and must answer then.
    void (*health)(void* user, unsigned token) = nullptr;
    void (*health_timeout)(void* user, unsigned token) = nullptr;
    unsigned (*outputs)(void* user, control_output* out, unsigned cap) = nullptr;
    unsigned (*windows)(void* user, control_window* out, unsigned cap) = nullptr;
    unsigned (*workspaces)(void* user, control_workspace* out, unsigned cap) = nullptr;
    /// The focused window (false: none).
    bool (*focused)(void* user, control_window& out) = nullptr;
    /// A Reload arrived. The host re-reads the configuration and answers with
    /// `control_server::finish_reload(token, …)` once the generation is applied (a keymap
    /// compiles on a worker first) — or at once.
    void (*reload)(void* user, unsigned token) = nullptr;
    control_action_result (*run_action)(void* user, const char* action,
                                        const char* argument, const char*& message) = nullptr;
    void (*logout)(void* user, const char* source) = nullptr;
    void* user = nullptr;
};

/// Peer credentials a test connection pretends to have.
struct control_peer {
    unsigned uid = 0;
    unsigned gid = 0;
    int pid = 0;
};

class control_server {
public:
    static constexpr unsigned k_max_connections = 16;
    static constexpr unsigned k_max_subscribers = 4;
    /// Stream events a subscriber may be sent while its socket stays full (it does not read)
    /// before it is dropped.
    static constexpr unsigned k_max_backlog = 64;
    static constexpr unsigned k_max_pending_health = 8;
    static constexpr unsigned k_max_pending_reload = 4;
    static constexpr unsigned health_timeout_ms = 500;

    control_server() = default;
    ~control_server();
    control_server(const control_server&) = delete;
    control_server& operator=(const control_server&) = delete;

    /// Creates the sd-event loop and the Varlink server (no socket yet). The user bus and
    /// anything else on sd-event can attach to `event()` from here on.
    [[nodiscard]] lx::result<void> open(control_host host);
    /// Listens on `path` (mode 0600). A file already there is removed first: the caller
    /// holds the Wayland display's lock, so the name is ours.
    [[nodiscard]] lx::result<void> listen(const char* path);
    /// A connection over an existing socket, as if from `peer` (tests: a socketpair).
    [[nodiscard]] lx::result<void> add_connection(int fd, const control_peer* peer);

    /// The `sd_event*` (null before `open`).
    [[nodiscard]] void* event() const { return event_; }
    /// sd-event's epoll fd: readable whenever anything on the loop has work.
    [[nodiscard]] int fd() const;
    /// Something was queued outside a dispatch (a reply, a stream event): the loop must
    /// dispatch once without waiting for the fd.
    [[nodiscard]] bool kicked() const { return kicked_; }
    /// Runs the sd-event loop until it has nothing to do. Never blocks.
    void dispatch();

    /// Answers the GetHealth `token` (ignored when it was answered or its client left).
    void finish_health(unsigned token, const lx::health::health_report& report);
    /// Answers the Reload `token`: the generation now in use, whether this reload's files
    /// were applied (false: rejected, the previous configuration stays), and its problems.
    void finish_reload(unsigned token, unsigned generation, bool applied,
                       std::span<const control_diagnostic> diagnostics);

    /// Stream events to every subscriber (Subscribe with `more`). No-ops without one.
    void publish_focus(const control_window* focused);
    /// `event`: "window-opened", "window-closed", "window-changed".
    void publish_window(const char* event, const control_window& w);
    /// `event`: "workspace" (created or changed: activated, renamed, moved, its window count),
    /// "workspace-removed" (only `id` meaningful).
    void publish_workspace(const char* event, const control_workspace& w);
    void publish_health(lx::health::level level);
    void publish_config(unsigned generation);
    [[nodiscard]] unsigned subscribers() const;

    /// Stops listening, drops every connection, removes the socket file, frees the loop.
    void close();
    [[nodiscard]] const char* path() const { return path_; }

    /// Internal (sd-varlink callbacks in control.cpp).
    struct impl;

private:
    friend struct impl;
    void* event_ = nullptr;
    void* server_ = nullptr;
    void* interface_ = nullptr;
    control_host host_{};
    bool kicked_ = false;
    char path_[256]{};

    struct pending_health {
        void* link = nullptr;
        void* timer = nullptr;
        unsigned generation = 0;
        control_server* owner = nullptr;
    };
    pending_health health_[k_max_pending_health]{};
    unsigned health_generation_ = 1;
    struct pending_reload {
        void* link = nullptr;
        unsigned generation = 0;
    };
    pending_reload reload_[k_max_pending_reload]{};
    unsigned reload_generation_ = 1;

    struct subscriber {
        void* link = nullptr;
        unsigned backlog = 0;
        /// Output was still unwritten after the last dispatch: the socket is full.
        bool stalled = false;
    };
    subscriber subscribers_[k_max_subscribers]{};
};

/// The interface definition in Varlink IDL (what `varlinkctl introspect` prints).
[[nodiscard]] const char* control_interface_idl();

} // namespace lx::compositor
